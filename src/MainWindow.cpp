#include "MainWindow.h"

#include <QCheckBox>
#include <QApplication>
#include <QClipboard>
#include <QMouseEvent>
#include <QWindow>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoWidget>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(tr("StreamingApp"));
    resize(1080, 760);
    setMinimumSize(720, 560);
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);
    setCentralWidget(central);

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(tr("Guardiamo insieme"), central);
    auto font = title->font(); font.setPointSize(20); font.setBold(true); title->setFont(font);
    auto* settingsButton = new QPushButton(tr("Impostazioni"), central);
    header->addWidget(title); header->addStretch(); header->addWidget(settingsButton);
    layout->addLayout(header);

    auto* roomFields = new QFormLayout;
    m_roomName = new QLineEdit(central);
    m_roomToken = new QLineEdit(central);
    m_roomToken->setObjectName(QStringLiteral("serverKeyEdit"));
    m_roomToken->setEchoMode(QLineEdit::Password);
    m_roomToken->setPlaceholderText(tr("Chiave del server, necessaria solo per creare stanze"));
    m_roomName->setPlaceholderText(tr("Incolla qui l’invito ricevuto"));
    roomFields->addRow(tr("Invito"), m_roomName);
    layout->addLayout(roomFields);
    auto* roomButtons = new QHBoxLayout;
    m_joinRoomButton = new QPushButton(tr("Entra"), central);
    m_createRoomButton = new QPushButton(tr("Crea stanza"), central);
    m_copyInviteButton = new QPushButton(tr("Copia invito"), central);
    m_copyInviteButton->hide();
    m_leaveRoomButton = new QPushButton(tr("Esci"), central);
    m_leaveRoomButton->setEnabled(false);
    m_leaveRoomButton->hide();
    m_roomStatusLabel = new QLabel(tr("Crea una stanza oppure incolla un invito per entrare."), central);
    m_roomStatusLabel->setTextFormat(Qt::PlainText);
    m_roomStatusLabel->setWordWrap(true);
    roomButtons->addWidget(m_createRoomButton); roomButtons->addWidget(m_joinRoomButton);
    roomButtons->addWidget(m_copyInviteButton); roomButtons->addWidget(m_leaveRoomButton);
    roomButtons->addWidget(m_roomStatusLabel, 1);
    layout->addLayout(roomButtons);

    m_videoArea = new QStackedWidget(central);
    auto* empty = new QLabel(tr("Il vostro schermo, qui\n\nCondividi l’invito. Poi uno di voi sceglie Condividi schermo."), m_videoArea);
    empty->setAlignment(Qt::AlignCenter); empty->setWordWrap(true);
    empty->setStyleSheet(QStringLiteral("background: #161b22; color: #d7dce2; padding: 24px; border-radius: 8px;"));
    m_videoArea->addWidget(empty);
    m_remoteWidget = new QVideoWidget(m_videoArea);
    m_remoteWidget->setAspectRatioMode(Qt::KeepAspectRatio);
    m_remoteWidget->setMinimumSize(320, 180);
    m_remoteWidget->setMouseTracking(true);
    m_remoteWidget->setToolTip(tr("Doppio click per tutto schermo · Esc per uscire"));
    // Qt Multimedia renders through a child QWindow. Observe application
    // events too: native video surfaces can consume QWidget mouse events.

    m_videoArea->addWidget(m_remoteWidget);
    layout->addWidget(m_videoArea, 1);

    auto* sourceRow = new QHBoxLayout;
    m_screenSelector = new QComboBox(central);
    m_screenSelector->setAccessibleName(tr("Schermo da condividere"));
    m_shareAudio = new QCheckBox(tr("Audio del PC"), central);
    m_shareAudio->setChecked(true);
    m_shareAudio->setToolTip(tr("Condivide l’audio dell’uscita predefinita del PC. Il microfono è escluso."));
    sourceRow->addWidget(new QLabel(tr("Schermo"), central));
    sourceRow->addWidget(m_screenSelector, 1); sourceRow->addWidget(m_shareAudio);
    layout->addLayout(sourceRow);
    auto* controls = new QHBoxLayout;
    m_shareButton = new QPushButton(tr("Condividi schermo"), central);
    m_shareButton->setEnabled(false);
    m_shareButton->setMinimumHeight(38);
    m_stopButton = new QPushButton(tr("Ferma condivisione"), central);
    m_stopButton->hide();
    m_volume = new QSlider(Qt::Horizontal, central);
    m_volume->setRange(0, 100); m_volume->setValue(100); m_volume->setMaximumWidth(130);
    m_volume->setAccessibleName(tr("Volume ricevuto"));
    m_volume->setToolTip(tr("Volume dello streaming ricevuto · 0 per silenziare"));
    controls->addWidget(m_shareButton); controls->addWidget(m_stopButton); controls->addStretch();
    controls->addWidget(new QLabel(tr("Volume"), central)); controls->addWidget(m_volume);
    layout->addLayout(controls);
    m_statusLabel = new QLabel(central);
    m_statusLabel->setTextFormat(Qt::PlainText); m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);
    m_errorLabel = new QLabel(central);
    m_errorLabel->setTextFormat(Qt::PlainText); m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(QStringLiteral("color: #b64332;"));
    m_errorLabel->hide(); layout->addWidget(m_errorLabel);

    auto* settings = new QDialog(this);
    settings->setWindowTitle(tr("Impostazioni"));
    settings->resize(540, 540);
    auto* settingsLayout = new QVBoxLayout(settings);
    auto* form = new QFormLayout;
    m_signalingUrl = new QLineEdit(settings);
    m_signalingUrl->setPlaceholderText(tr("wss://nome-del-server:8443"));
    m_fpsSelector = new QComboBox(settings);
    m_fpsSelector->addItem(tr("30 FPS"), 30); m_fpsSelector->addItem(tr("60 FPS"), 60);
    m_fpsSelector->setCurrentIndex(1);
    m_resolutionSelector = new QComboBox(settings);
    m_resolutionSelector->addItem(tr("720p"), QSize(1280, 720));
    m_resolutionSelector->addItem(tr("1080p"), QSize(1920, 1080));
    m_resolutionSelector->setCurrentIndex(1);
    m_bitrate = new QSpinBox(settings);
    m_bitrate->setRange(1, 50); m_bitrate->setValue(12); m_bitrate->setSuffix(tr(" Mbit/s"));
    form->addRow(tr("Server per creare stanze"), m_signalingUrl);
    form->addRow(tr("Chiave del server"), m_roomToken);
    m_rememberServerKey = new QCheckBox(tr("Ricorda la chiave su questo PC"), settings);
    m_rememberServerKey->setObjectName(QStringLiteral("rememberServerKey"));
    m_rememberServerKey->setToolTip(tr("Salva la chiave nelle preferenze locali del tuo utente, non cifrata. Disattiva per rimuoverla."));
    form->addRow(m_rememberServerKey);
    form->addRow(tr("Qualità massima"), m_resolutionSelector);
    form->addRow(tr("Fluidità"), m_fpsSelector);
    form->addRow(tr("Bitrate video"), m_bitrate);
    settingsLayout->addLayout(form);
    auto* hint = new QLabel(tr("Chi riceve l’invito non deve configurare server o chiave. La qualità rispetta i limiti scelti da entrambi. Per cambiarli, esci e rientra nella stanza."), settings);
    hint->setWordWrap(true); settingsLayout->addWidget(hint);
    auto* advancedToggle = new QPushButton(tr("Rete e diagnostica ▾"), settings);
    advancedToggle->setCheckable(true); settingsLayout->addWidget(advancedToggle);
    auto* advanced = new QWidget(settings);
    auto* advancedForm = new QFormLayout(advanced);
    m_stunServer = new QLineEdit(QStringLiteral("stun://stun.l.google.com:19302"), advanced);
    m_turnServer = new QLineEdit(advanced);
    m_turnServer->setPlaceholderText(tr("turn://utente:password@host:3478"));
    m_turnServer->setEchoMode(QLineEdit::Password);
    m_relayOnly = new QCheckBox(tr("Usa solo TURN"), advanced);
    advancedForm->addRow(tr("STUN"), m_stunServer);
    advancedForm->addRow(tr("TURN"), m_turnServer);
    advancedForm->addRow(m_relayOnly);
    m_statisticsLabel = new QLabel(advanced); m_statisticsLabel->setWordWrap(true);
    m_timingsLabel = new QLabel(advanced); m_timingsLabel->setWordWrap(true);
    advancedForm->addRow(m_statisticsLabel); advancedForm->addRow(m_timingsLabel);
    settingsLayout->addWidget(advanced); advanced->hide();
    connect(advancedToggle, &QPushButton::toggled, advanced, &QWidget::setVisible);
    settingsLayout->addStretch();
    auto* close = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close, settings);
    close->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("saveSettingsButton"));
    close->button(QDialogButtonBox::Save)->setText(tr("Salva impostazioni"));
    connect(close, &QDialogButtonBox::accepted, this, [this] {
        if (saveSettings()) m_statusLabel->setText(tr("Impostazioni salvate."));
        else { m_errorLabel->setText(tr("Impossibile salvare le impostazioni sul PC.")); m_errorLabel->show(); }
    });
    connect(close, &QDialogButtonBox::rejected, settings, &QDialog::hide);
    settingsLayout->addWidget(close);
    connect(settingsButton, &QPushButton::clicked, settings, [settings] { settings->show(); settings->raise(); settings->activateWindow(); });

    QSettings saved(QStringLiteral("StreamingApp"), QStringLiteral("StreamingApp"));
    m_signalingUrl->setText(saved.value("server", "ws://127.0.0.1:8443").toString());

    m_rememberServerKey->setChecked(saved.value("rememberServerKey", false).toBool());
    if (m_rememberServerKey->isChecked()) m_roomToken->setText(saved.value("serverKey").toString());
    connect(m_rememberServerKey, &QCheckBox::toggled, this, [this](bool remember) {
        if (!remember) {
            QSettings preferences(QStringLiteral("StreamingApp"), QStringLiteral("StreamingApp"));
            preferences.remove("serverKey"); preferences.setValue("rememberServerKey", false); preferences.sync();
        }
    });
    m_resolutionSelector->setCurrentIndex(saved.value("resolution", 1).toInt() == 0 ? 0 : 1);
    m_fpsSelector->setCurrentIndex(saved.value("fps", 1).toInt() == 0 ? 0 : 1);
    m_bitrate->setValue(saved.value("bitrate", 12).toInt());

    m_controller = std::make_unique<ScreenCaptureController>(m_remoteWidget->videoSink());
    m_room = std::make_unique<RoomClient>(this);
    refreshScreens();
    connect(qGuiApp, &QGuiApplication::screenAdded, this, &MainWindow::refreshScreens);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &MainWindow::refreshScreens);
    connect(m_stopButton, &QPushButton::clicked, m_room.get(), &RoomClient::stopSharing);
    connect(m_controller.get(), &ScreenCaptureController::stateChanged, this, [this](auto state) {
        updateState(state);
        if (state == ScreenCaptureController::State::Idle && m_room->sending()) {
            QTimer::singleShot(0, this, [this] {
                if (m_room->sending() && m_controller->state() == ScreenCaptureController::State::Idle)
                    m_room->stopSharing();
            });
        }
    });
    connect(m_controller.get(), &ScreenCaptureController::errorOccurred, this, [this](const QString& error) {
        m_room->stopSharing();
        m_errorLabel->setText(tr("Cattura non riuscita: %1").arg(error)); m_errorLabel->show();
    });
    connect(m_controller.get(), &ScreenCaptureController::statisticsChanged, this,
        [this](double captured, double decoded, QSize size, double latency, quint64 skipped) {
            m_statisticsLabel->setText(tr("Cattura %1 FPS · Decodifica %2 FPS\n%3 × %4 · Ritardo locale %5 ms · Frame saltati %6")
                .arg(captured, 0, 'f', 1).arg(decoded, 0, 'f', 1).arg(size.width()).arg(size.height())
                .arg(latency, 0, 'f', 1).arg(skipped));
        });
    connect(m_controller.get(), &ScreenCaptureController::timingsChanged, this,
        [this](double prepare, double encode, double decode, double output) {
            m_timingsLabel->setText(tr("Preparazione %1 ms · Codifica %2 ms · Decodifica %3 ms · Uscita %4 ms")
                .arg(prepare, 0, 'f', 1).arg(encode, 0, 'f', 1).arg(decode, 0, 'f', 1).arg(output, 0, 'f', 1));
        });
    connect(m_joinRoomButton, &QPushButton::clicked, this, [this] {
        m_errorLabel->hide();
        const VideoSettings video{m_fpsSelector->currentData().toInt(), m_resolutionSelector->currentData().toSize(), m_bitrate->value() * 1000};
        const WebRtcPeer::IceConfig ice{m_stunServer->text().trimmed(), m_turnServer->text().trimmed(), m_relayOnly->isChecked()};
        m_room->joinInvitation(m_roomName->text(), video, ice);
    });
    connect(m_createRoomButton, &QPushButton::clicked, this, [this, settings] {
        m_errorLabel->hide();
        if (m_roomToken->text().size() < 16) {
            settings->show(); settings->raise(); m_roomToken->setFocus();
            m_statusLabel->setText(tr("Configura server e chiave nelle impostazioni, poi premi Crea stanza."));
            return;
        }
        const VideoSettings video{m_fpsSelector->currentData().toInt(), m_resolutionSelector->currentData().toSize(), m_bitrate->value() * 1000};
        const WebRtcPeer::IceConfig ice{m_stunServer->text().trimmed(), m_turnServer->text().trimmed(), m_relayOnly->isChecked()};
        m_room->create(QUrl(m_signalingUrl->text().trimmed()), m_roomToken->text(), video, ice);
    });
    connect(m_roomName, &QLineEdit::returnPressed, m_joinRoomButton, &QPushButton::click);
    connect(m_room.get(), &RoomClient::invitationReady, this, [this](const QString& invitation) {
        m_roomName->setText(invitation);
        m_roomName->setCursorPosition(0);
        m_copyInviteButton->setEnabled(true);
    });
    connect(m_copyInviteButton, &QPushButton::clicked, this, [this] {
        m_roomName->setFocus();
        m_roomName->selectAll();
        QGuiApplication::clipboard()->setText(m_roomName->text());
        m_statusLabel->setText(tr("Invito copiato. Invialo all’altra persona; contiene l’accesso alla stanza."));
    });
    connect(m_leaveRoomButton, &QPushButton::clicked, m_room.get(), &RoomClient::leave);
    connect(m_shareButton, &QPushButton::clicked, this, [this] {
        m_errorLabel->hide();
        m_room->setShareAudio(m_shareAudio->isChecked()); m_room->publish();
    });
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) { m_room->setVolume(value / 100.0); });
    connect(m_room.get(), &RoomClient::captureRequested, this, [this](const VideoSettings& video) {
        const int index = m_screenSelector->currentIndex();
        QScreen* screen = index >= 0 && index < m_screens.size() ? m_screens[index].data() : nullptr;
        m_controller->start(screen, video, m_room->encodedHandler());
    });
    connect(m_room.get(), &RoomClient::sessionStopped, this, [this] {
        m_controller->stop(); m_remoteWidget->videoSink()->setVideoFrame({});
        m_remoteWidget->setFullScreen(false); m_videoArea->setCurrentIndex(0);
    });
    connect(m_room.get(), &RoomClient::transportConnected, this, [this] {
        if (!m_room->sending()) m_statusLabel->setText(tr("Doppio click sul video per guardare a tutto schermo."));
    });
    connect(m_room.get(), &RoomClient::remoteFrame, this, [this](const QVideoFrame& frame) {
        m_videoArea->setCurrentIndex(1); m_remoteWidget->videoSink()->setVideoFrame(frame);
    });
    connect(m_room.get(), &RoomClient::statusChanged, m_roomStatusLabel, &QLabel::setText);
    connect(m_room.get(), &RoomClient::errorOccurred, this, [this](const QString& error) {
        m_errorLabel->setText(error); m_errorLabel->show();
    });
    connect(m_room.get(), &RoomClient::changed, this, [this] {
        const bool locked = m_room->joined() || m_room->connecting();
        m_createRoomButton->setVisible(!locked); m_createRoomButton->setEnabled(!locked);
        m_copyInviteButton->setVisible(m_room->joined());
        m_joinRoomButton->setVisible(!locked); m_joinRoomButton->setEnabled(!locked);
        m_leaveRoomButton->setVisible(locked); m_leaveRoomButton->setEnabled(locked);
        m_shareButton->setEnabled(m_room->joined() && !m_room->busy() && m_room->participants() == 2
            && (m_controller->usesPortal() || !m_screens.isEmpty()));
        // Keep invitations selectable and copyable while preventing edits in a room.
        m_roomName->setReadOnly(locked);
        for (auto* edit : {m_signalingUrl, m_roomToken, m_stunServer, m_turnServer}) edit->setEnabled(!locked);
        m_relayOnly->setEnabled(!locked);
        updateState(m_controller->state());
    });

    m_fullscreenExitButton = new QPushButton(QStringLiteral("×"), m_remoteWidget);
    m_fullscreenExitButton->setToolTip(tr("Esci da tutto schermo"));
    m_fullscreenExitButton->setFixedSize(40, 40);
    m_fullscreenExitButton->hide();
    m_fullscreenHideTimer = new QTimer(this); m_fullscreenHideTimer->setSingleShot(true);
    connect(m_fullscreenExitButton, &QPushButton::clicked, this, [this] { m_remoteWidget->setFullScreen(false); });
    connect(m_fullscreenHideTimer, &QTimer::timeout, m_fullscreenExitButton, &QPushButton::hide);
    connect(m_remoteWidget, &QVideoWidget::fullScreenChanged, this, [this](bool full) {
        if (full) showFullscreenControl(); else m_fullscreenExitButton->hide();
    });
    m_controller->stop();
    updateState(m_controller->state());
    qApp->installEventFilter(this);
}

MainWindow::~MainWindow()
{
    saveSettings();
    qApp->removeEventFilter(this);
    if (m_remoteWidget->isFullScreen()) m_remoteWidget->setFullScreen(false);
}

bool MainWindow::saveSettings()
{
    QSettings saved(QStringLiteral("StreamingApp"), QStringLiteral("StreamingApp"));
    saved.setValue("server", m_signalingUrl->text());
    saved.setValue("resolution", m_resolutionSelector->currentIndex());
    saved.setValue("fps", m_fpsSelector->currentIndex()); saved.setValue("bitrate", m_bitrate->value());
    saved.setValue("rememberServerKey", m_rememberServerKey->isChecked());
    if (m_rememberServerKey->isChecked()) saved.setValue("serverKey", m_roomToken->text());
    else saved.remove("serverKey");
    saved.sync();
#ifdef Q_OS_UNIX
    if (saved.status() == QSettings::NoError && m_rememberServerKey->isChecked()
        && !QFile::setPermissions(saved.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return false;
#endif
    return saved.status() == QSettings::NoError;
}

void MainWindow::refreshScreens()
{
    const int index = m_screenSelector->currentIndex();
    QPointer<QScreen> previous = index >= 0 && index < m_screens.size() ? m_screens[index] : nullptr;
    m_screens.clear(); m_screenSelector->clear();
    if (m_controller->usesPortal()) {
        m_screenSelector->addItem(tr("Scegli nel dialogo del sistema all’avvio"));
        m_screenSelector->setEnabled(false); return;
    }
    for (auto* screen : QGuiApplication::screens()) {
        m_screens.append(screen);
        const auto size = screen->size() * screen->devicePixelRatio();
        m_screenSelector->addItem(tr("%1 · %2 × %3%4").arg(screen->name()).arg(size.width()).arg(size.height())
            .arg(screen == QGuiApplication::primaryScreen() ? tr(" · principale") : QString()));
        if (screen == previous) m_screenSelector->setCurrentIndex(m_screens.size() - 1);
    }
    if (m_room) {
        m_shareButton->setEnabled(m_room->joined() && !m_room->busy() && m_room->participants() == 2 && !m_screens.isEmpty());
        updateState(m_controller->state());
    }
}

void MainWindow::updateState(ScreenCaptureController::State state)
{
    using State = ScreenCaptureController::State;
    const bool busy = m_room->busy();
    const bool locked = m_room->joined() || m_room->connecting();
    m_stopButton->setVisible(busy); m_stopButton->setEnabled(busy);
    m_shareButton->setVisible(!busy);
    m_screenSelector->setEnabled(!busy && !m_controller->usesPortal());
    m_shareAudio->setEnabled(!busy);
    m_fpsSelector->setEnabled(!locked); m_resolutionSelector->setEnabled(!locked); m_bitrate->setEnabled(!locked);
    m_volume->setEnabled(busy && !m_room->sending());
    switch (state) {
    case State::Idle:
        m_statusLabel->setText(busy ? tr("Collegamento in corso…") : tr("Potete condividere a turno. Ferma la condivisione per passare la parola.")); break;
    case State::Starting:
        m_statusLabel->setText(m_controller->usesPortal() ? tr("Scegli lo schermo nel dialogo del sistema…") : tr("Avvio della condivisione…")); break;
    case State::Capturing:
        m_videoArea->setCurrentIndex(1); m_statusLabel->setText(tr("Anteprima del tuo schermo")); break;
    case State::Error: break;
    }
}

void MainWindow::showFullscreenControl()
{
    if (!m_remoteWidget->isFullScreen()) return;
    m_fullscreenExitButton->move(m_remoteWidget->width() - 56, 16);
    m_fullscreenExitButton->show(); m_fullscreenExitButton->raise(); m_fullscreenHideTimer->start(2000);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    // Test global coordinates as the video may be hosted by a native child
    // window instead of delivering events to QVideoWidget itself.
    const bool videoVisible = m_remoteWidget && m_remoteWidget->isVisible();
    const bool ownWindow = QApplication::activeWindow() == this
        || QApplication::activeWindow() == m_remoteWidget;
    if (videoVisible && ownWindow) {
        if (event->type() == QEvent::MouseButtonDblClick || event->type() == QEvent::MouseMove) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const QPoint local = m_remoteWidget->mapFromGlobal(mouse->globalPosition().toPoint());
            if (m_remoteWidget->rect().contains(local)) {
                if (event->type() == QEvent::MouseButtonDblClick && mouse->button() == Qt::LeftButton) {
                    m_remoteWidget->setFullScreen(!m_remoteWidget->isFullScreen()); return true;
                }
                showFullscreenControl();
            }
        }
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape
            && m_remoteWidget->isFullScreen()) {
            m_remoteWidget->setFullScreen(false); return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

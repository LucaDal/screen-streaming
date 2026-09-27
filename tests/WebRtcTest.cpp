#include "WebRtcPeer.h"
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QVideoFrameFormat>
#include <QImage>
#include <QHostAddress>
#include <QNetworkInterface>
#include <algorithm>
#include <iostream>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const auto addresses = QNetworkInterface::allAddresses();
    const bool hasRoutableAddress = std::any_of(addresses.cbegin(), addresses.cend(), [](const QHostAddress& address) {
        return !address.isNull() && !address.isLoopback();
    });
    if (!hasRoutableAddress) {
        std::cerr << "SKIP: no routable network interface is available in this environment\n";
        return 77;
    }
    const bool systemAudio = app.arguments().contains("--system-audio");
    const bool withAudio = systemAudio || app.arguments().contains("--audio");
    WebRtcPeer sender, receiver;
    QString failure;
    bool connectedSender = false, connectedReceiver = false, encryptedOffer = false;
    int senderMessages = 0, receiverMessages = 0;
    QObject::connect(&sender, &WebRtcPeer::signalMessage, &receiver, [&](const QJsonObject& m) {
        ++senderMessages;
        if (m.value("type") == "offer") {
            const auto sdp = m.value("sdp").toString();
            encryptedOffer = sdp.contains("UDP/TLS/RTP/SAVPF") && sdp.contains("a=fingerprint:sha-256")
                && (!withAudio || (sdp.contains("m=audio") && sdp.contains("OPUS/48000")));
        }
        receiver.receive(m);
    });
    QObject::connect(&receiver, &WebRtcPeer::signalMessage, &sender, [&](const QJsonObject& m) {
        ++receiverMessages;
        sender.receive(m);
    });
    QObject::connect(&sender, &WebRtcPeer::errorOccurred, &app, [&](const QString& e) { failure = e; });
    QObject::connect(&receiver, &WebRtcPeer::errorOccurred, &app, [&](const QString& e) { failure = e; });
    QObject::connect(&sender, &WebRtcPeer::connected, &app, [&] { connectedSender = true; });
    QObject::connect(&receiver, &WebRtcPeer::connected, &app, [&] { connectedReceiver = true; });
    QVideoFrame received;
    QObject::connect(&receiver, &WebRtcPeer::frameReady, &app, [&](const QVideoFrame& frame) { received = frame; });
    QString error;
    if (!receiver.start(false, {}, {withAudio, false, true}, error) || !sender.start(true, {}, {withAudio, !systemAudio, false}, error)) {
        std::cerr << error.toStdString() << '\n'; return 1;
    }
    VideoPipeline pipeline;
    if (!pipeline.start({60, QSize(320, 180), 2000}, error, sender.encodedHandler())) {
        std::cerr << error.toStdString() << '\n'; return 1;
    }
    QVideoFrame input(QVideoFrameFormat(QSize(320, 180), QVideoFrameFormat::Format_RGBA8888));
    input.map(QVideoFrame::WriteOnly);
    for (int y = 0; y < 180; ++y) for (int x = 0; x < 320; ++x) {
        auto* p = input.bits(0) + y * input.bytesPerLine(0) + x * 4;
        p[0] = 200; p[1] = 70; p[2] = 30; p[3] = 255;
    }
    input.unmap();
    QElapsedTimer timer; timer.start(); qint64 next = 0;
    while (timer.elapsed() < 20000 && (receiver.receivedFrames() < 10 || (withAudio && timer.elapsed() < 6000)) && failure.isEmpty()) {
        app.processEvents();
        if (timer.elapsed() >= next && (!withAudio || timer.elapsed() > 1500)) { pipeline.submit(input); next = timer.elapsed() + 16; }
        const auto snapshot = pipeline.takeLatest();
        if (!snapshot.error.isEmpty()) failure = snapshot.error;
        QThread::msleep(2);
    }
    const auto frames = receiver.receivedFrames();
    const auto media = receiver.mediaStats();
    if (withAudio && (media.audioBuffers < 50 || media.videoPtsNs < 0 || media.audioPtsNs < 0
        || std::abs(media.videoPtsNs - media.audioPtsNs) > 250000000)) {
        failure = QString("A/V timestamps diverged: video=%1 audio=%2 buffers=%3")
            .arg(media.videoPtsNs).arg(media.audioPtsNs).arg(media.audioBuffers);
    }
    pipeline.stop(); sender.stop(); receiver.stop();
    if (!failure.isEmpty() || !connectedSender || !connectedReceiver || !encryptedOffer || frames < 10 || !received.isValid()) {
        std::cerr << "FAIL: " << failure.toStdString() << "; sender=" << connectedSender
                  << " receiver=" << connectedReceiver << " encrypted SDP=" << encryptedOffer << " frames=" << frames
                  << " messages=" << senderMessages << '/' << receiverMessages << '\n'; return 1;
    }
    const auto image = received.toImage();
    const auto color = image.pixelColor(160, 90);
    if (std::abs(color.red() - 200) > 20 || std::abs(color.green() - 70) > 20 || std::abs(color.blue() - 30) > 20) {
        std::cerr << "FAIL: received pixel mismatch\n"; return 1;
    }
    std::cout << "Audio buffers=" << media.audioBuffers << " A/V offset ms=" << (media.videoPtsNs - media.audioPtsNs) / 1000000 << "\n";
    std::cout << "PASS: two WebRTC peers, ICE/DTLS-SRTP, real H.264 frames, pixel checks and retained frame after stop\n";
}

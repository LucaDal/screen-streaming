#include "SignalingServer.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslConfiguration>
#include <QTextStream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOptions({{{"p", "port"}, "Porta", "port", "8443"},
                       {"token-file", "File con chiave per creare stanze (almeno 16 caratteri)", "path"},
                       {"cert", "Certificato TLS PEM", "path"}, {"key", "Chiave privata RSA PEM", "path"},
                       {"insecure-local", "Solo test sullo stesso computer: WS su 127.0.0.1"}});
    parser.process(app);
    QFile tokenFile(parser.value("token-file"));
    if (!tokenFile.open(QIODevice::ReadOnly)) { qCritical("Serve --token-file."); return 1; }
    const QString token = QString::fromUtf8(tokenFile.readAll()).trimmed();
    if (token.size() < 16 || token.size() > 1024) { qCritical("Il token deve contenere 16–1024 caratteri."); return 1; }
    bool portOk = false; const auto port = parser.value("port").toUInt(&portOk);
    if (!portOk || port == 0 || port > 65535) { qCritical("Porta non valida."); return 1; }
    const bool local = parser.isSet("insecure-local");
    SignalingServer server(token, local ? QWebSocketServer::NonSecureMode : QWebSocketServer::SecureMode);
    if (!local) {
        QFile certFile(parser.value("cert")), keyFile(parser.value("key"));
        if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) { qCritical("Per WSS servono --cert e --key."); return 1; }
        const auto chain = QSslCertificate::fromData(certFile.readAll());
        const QSslKey key(keyFile.readAll(), QSsl::Rsa);
        if (chain.isEmpty() || key.isNull() || !QSslSocket::supportsSsl()) { qCritical("Configurazione TLS non valida."); return 1; }
        auto config = QSslConfiguration::defaultConfiguration();
        config.setLocalCertificateChain(chain); config.setPrivateKey(key);
        config.setPeerVerifyMode(QSslSocket::VerifyNone); // Clients authenticate using the room token.
        config.setProtocol(QSsl::TlsV1_2OrLater);
        server.setSslConfiguration(config);
    }
    if (!server.listen(local ? QHostAddress::LocalHost : QHostAddress::Any, static_cast<quint16>(port))) {
        qCritical().noquote() << server.errorString(); return 1;
    }
    QTextStream(stdout) << "Signaling " << (local ? "ws://127.0.0.1:" : "wss://<host>:") << port << '\n';
    return app.exec();
}

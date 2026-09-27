#include "SignalingServer.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslConfiguration>
#include <QTextStream>
#include <QTimer>
#include <csignal>

namespace {
volatile std::sig_atomic_t stopRequested = 0;
void requestStop(int) { stopRequested = 1; }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOptions({{{"p", "port"}, "Porta", "port", "8443"},
                       {"check-runtime", "Verifica il supporto TLS senza avviare il server"},
                       {"token-file", "File con chiave per creare stanze (almeno 16 caratteri)", "path"},
                       {"turn-config", "Avvia e gestisci coturn usando questo file di configurazione", "path"},
                       {"turn-url", "URL TURN pubblico senza credenziali (ripetibile, UDP/TCP)", "url"},
                       {"turn-executable", "Eseguibile coturn", "path", "turnserver"},
                       {"cert", "Certificato TLS PEM", "path"}, {"key", "Chiave privata PEM (RSA o EC, senza passphrase)", "path"},
                       {"insecure-local", "Solo test sullo stesso computer: WS su 127.0.0.1"}});
    parser.process(app);
    if (parser.isSet("check-runtime")) {
        if (!QSslSocket::supportsSsl()) { qCritical("Supporto TLS non disponibile in Qt."); return 1; }
        QTextStream(stdout) << "Runtime TLS disponibile.\n";
        return 0;
    }
    QFile tokenFile(parser.value("token-file"));
    if (!tokenFile.open(QIODevice::ReadOnly)) { qCritical("Serve --token-file."); return 1; }
    const QString token = QString::fromUtf8(tokenFile.readAll()).trimmed();
    if (token.size() < 16 || token.size() > 1024) { qCritical("Il token deve contenere 16–1024 caratteri."); return 1; }
    bool portOk = false; const auto port = parser.value("port").toUInt(&portOk);
    if (!portOk || port == 0 || port > 65535) { qCritical("Porta non valida."); return 1; }
    const bool local = parser.isSet("insecure-local");
    TurnService turn;
    SignalingServer server(token, local ? QWebSocketServer::NonSecureMode : QWebSocketServer::SecureMode);
    if (!local) {
        if (!QSslSocket::supportsSsl()) { qCritical("Supporto TLS non disponibile in Qt."); return 1; }
        QFile certFile(parser.value("cert")), keyFile(parser.value("key"));
        if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) { qCritical("Per WSS servono --cert e --key."); return 1; }
        const auto chain = QSslCertificate::fromData(certFile.readAll());
        if (chain.isEmpty()) { qCritical("Il file --cert non contiene un certificato PEM valido."); return 1; }
        // The leaf certificate and its private key must use the same algorithm.
        const auto publicKey = chain.first().publicKey();
        if (publicKey.isNull()) { qCritical("Chiave pubblica del certificato TLS non supportata."); return 1; }
        const QSslKey key(keyFile.readAll(), publicKey.algorithm(), QSsl::Pem, QSsl::PrivateKey);
        if (key.isNull()) { qCritical("Chiave privata TLS non valida: serve un PEM senza passphrase con lo stesso algoritmo del certificato."); return 1; }
        auto config = QSslConfiguration::defaultConfiguration();
        config.setLocalCertificateChain(chain); config.setPrivateKey(key);
        config.setPeerVerifyMode(QSslSocket::VerifyNone); // Clients authenticate using the room token.
        config.setProtocol(QSsl::TlsV1_2OrLater);
        server.setSslConfiguration(config);
    }
    if (parser.isSet("turn-config")) {
        QString error;
        if (!turn.configure(parser.value("turn-config"), parser.values("turn-url"), error)) {
            qCritical().noquote() << error; return 1;
        }
        server.setTurnService(&turn);
    } else if (parser.isSet("turn-url") || parser.isSet("turn-executable")) {
        qCritical("--turn-url e --turn-executable richiedono --turn-config."); return 1;
    }
    if (!server.listen(local ? QHostAddress::LocalHost : QHostAddress::Any, static_cast<quint16>(port))) {
        qCritical().noquote() << server.errorString(); return 1;
    }
    if (parser.isSet("turn-config")) {
        QString error;
        if (!turn.start(parser.value("turn-executable"), error)) {
            qCritical().noquote() << error; return 1;
        }
        QObject::connect(&turn, &TurnService::failed, &app, [&app] {
            qCritical("Coturn si è arrestato: arresto anche il signaling. Controlla i log e riavvia il servizio.");
            app.exit(1);
        });
        QTextStream(stdout) << "TURN gestito attivo; credenziali automatiche per ogni sessione.\n";
    }
    // Let Qt unwind normally so its child coturn is terminated on service stop.
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    QTimer shutdownPoll;
    QObject::connect(&shutdownPoll, &QTimer::timeout, &app, [&app] { if (stopRequested) app.quit(); });
    shutdownPoll.start(100);
    QTextStream(stdout) << "Signaling " << (local ? "ws://127.0.0.1:" : "wss://<host>:") << port << '\n';
    return app.exec();
}

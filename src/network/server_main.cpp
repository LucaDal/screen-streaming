#include "SignalingServer.h"
#include "ServerConfig.h"
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
    // A command-line server must report startup errors in its terminal, even
    // on Qt builds which otherwise send diagnostics only to journald.
    if (!qEnvironmentVariableIsSet("QT_FORCE_STDERR_LOGGING"))
        qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QCoreApplication app(argc, argv);
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOptions({{{"p", "port"}, "Porta", "port", "8443"},
                       {"config", "File INI del signaling (le opzioni CLI hanno precedenza)", "path"},
                       {"check-runtime", "Verifica il supporto TLS senza avviare il server"},
                       {"token", "Chiave per creare stanze (alternativa a token-file)", "value"},
                       {"token-file", "File con chiave per creare stanze (almeno 16 caratteri)", "path"},
                       {"server-url", "URL pubblico WSS: genera e avvia coturn dal dominio (IPv4)", "url"},
                       {"turn-relay-ip", "IPv4 locale per TURN automatico (predefinito: rilevato dalla rete)", "ip"},
                       {"turn-port", "Porta TURN automatico UDP/TCP", "port", "3478"},
                       {"turn-min-port", "Prima porta UDP relay automatico", "port", "49160"},
                       {"turn-max-port", "Ultima porta UDP relay automatico", "port", "49260"},
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
    QString configError;
    if (!applyServerConfig(parser, app.arguments(), configError)) {
        qCritical().noquote() << configError; return 1;
    }
    if (parser.isSet("token") == parser.isSet("token-file")) {
        qCritical("Specifica una sola opzione tra token (valore diretto) e token-file (percorso del file)."); return 1;
    }
    QString token;
    if (parser.isSet("token")) {
        token = parser.value("token").trimmed();
    } else {
        QFile tokenFile(parser.value("token-file"));
        if (!tokenFile.open(QIODevice::ReadOnly)) {
            qCritical("Impossibile leggere token-file: usa token per un valore diretto, oppure token-file per il percorso di un file contenente il token."); return 1;
        }
        token = QString::fromUtf8(tokenFile.readAll()).trimmed();
    }
    if (token.size() < 16 || token.size() > 1024) { qCritical("Il token deve contenere 16–1024 caratteri."); return 1; }
    const bool automaticTurn = parser.isSet("server-url");
    const bool managedTurn = automaticTurn || parser.isSet("turn-config");
    const QUrl publicUrl(parser.value("server-url"), QUrl::StrictMode);
    if (automaticTurn && (parser.isSet("turn-config") || parser.isSet("turn-url") || parser.isSet("insecure-local"))) {
        qCritical("--server-url genera TURN automaticamente: non combinarlo con --turn-config, --turn-url o --insecure-local."); return 1;
    }
    if (!automaticTurn && (parser.isSet("turn-relay-ip") || parser.isSet("turn-port")
        || parser.isSet("turn-min-port") || parser.isSet("turn-max-port"))) {
        qCritical("Le opzioni TURN automatiche richiedono --server-url."); return 1;
    }
    bool portOk = false;
    const auto portText = !parser.isSet("port") && automaticTurn
        ? QString::number(publicUrl.port(443)) : parser.value("port");
    const auto port = portText.toUInt(&portOk);
    if (!portOk || port == 0 || port > 65535) { qCritical("Porta non valida."); return 1; }
    const bool local = parser.isSet("insecure-local");
    TurnService turn;
    SignalingServer server(token, local ? QWebSocketServer::NonSecureMode : QWebSocketServer::SecureMode);
    if (!local) {
        if (!QSslSocket::supportsSsl()) { qCritical("Supporto TLS non disponibile in Qt."); return 1; }
        QFile certFile(parser.value("cert")), keyFile(parser.value("key"));
        if (!certFile.open(QIODevice::ReadOnly)) {
            qCritical("Impossibile leggere cert: controlla il percorso del certificato PEM, i permessi e che il disco sia montato."); return 1;
        }
        if (!keyFile.open(QIODevice::ReadOnly)) {
            qCritical("Impossibile leggere key: controlla il percorso della chiave privata PEM, i permessi e che il disco sia montato."); return 1;
        }
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
    if (automaticTurn) {
        TurnService::AutomaticConfig options;
        options.serverUrl = publicUrl;
        auto readPort = [&](const char* name, quint16& target) {
            bool okay = false;
            const auto value = parser.value(QLatin1String(name)).toUInt(&okay);
            if (!okay || value == 0 || value > 65535) return false;
            target = static_cast<quint16>(value);
            return true;
        };
        if (!readPort("turn-port", options.port) || !readPort("turn-min-port", options.minPort)
            || !readPort("turn-max-port", options.maxPort)) {
            qCritical("Porte TURN non valide: specifica numeri tra 1 e 65535."); return 1;
        }
        if (options.port == port) { qCritical("Signaling e TURN devono usare porte di ascolto diverse."); return 1; }
        if (parser.isSet("turn-relay-ip") && !options.relayAddress.setAddress(parser.value("turn-relay-ip"))) {
            qCritical("--turn-relay-ip richiede un indirizzo IPv4 locale."); return 1;
        }
        QString error;
        if (!turn.configureAutomatic(options, error)) { qCritical().noquote() << error; return 1; }
        server.setTurnService(&turn);
    } else if (parser.isSet("turn-config")) {
        QString error;
        if (!turn.configure(parser.value("turn-config"), parser.values("turn-url"), error)) {
            qCritical().noquote() << error; return 1;
        }
        server.setTurnService(&turn);
    } else if (parser.isSet("turn-url") || parser.isSet("turn-executable")) {
        qCritical("--turn-url richiede --turn-config; --turn-executable richiede --turn-config o --server-url."); return 1;
    }
    if (!server.listen(local ? QHostAddress::LocalHost : QHostAddress::Any, static_cast<quint16>(port))) {
        qCritical().noquote() << server.errorString(); return 1;
    }
    if (managedTurn) {
        QString error;
        if (!turn.start(parser.value("turn-executable"), error)) {
            qCritical().noquote() << error; return 1;
        }
        QObject::connect(&turn, &TurnService::failed, &app, [&app] {
            qCritical("Coturn si è arrestato: arresto anche il signaling. Controlla i log e riavvia il servizio.");
            app.exit(1);
        });
        QTextStream(stdout) << "TURN gestito attivo; credenziali automatiche per ogni sessione.\n";
        if (automaticTurn)
            QTextStream(stdout) << "Configurazione TURN generata dal DNS all'avvio. Dopo un cambio IP, aggiorna il DNS e riavvia il signaling.\n";
    }
    // Let Qt unwind normally so its child coturn is terminated on service stop.
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    QTimer shutdownPoll;
    QObject::connect(&shutdownPoll, &QTimer::timeout, &app, [&app] { if (stopRequested) app.quit(); });
    shutdownPoll.start(100);
    if (automaticTurn)
        QTextStream(stdout) << "Signaling " << publicUrl.toString() << " (porta locale " << port << ")\n";
    else
        QTextStream(stdout) << "Signaling " << (local ? "ws://127.0.0.1:" : "wss://<host>:") << port << '\n';
    return app.exec();
}

#include "TurnService.h"
#include <QFile>
#include <QEventLoop>
#include <QHostInfo>
#include <QMessageAuthenticationCode>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QUrlQuery>

TurnService::TurnService(QObject* parent) : QObject(parent) {
    m_process.setProcessChannelMode(QProcess::ForwardedChannels);
    connect(&m_process, &QProcess::finished, this, [this] {
        if (!m_stopping) emit failed();
    });
}
TurnService::~TurnService() { stop(); }

bool TurnService::configure(const QString& configFile, const QStringList& urls, QString& error) {
    m_secret.clear(); m_urls.clear(); m_config.clear();
    QFile file(configFile);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) {
        error = tr("Impossibile leggere la configurazione coturn (massimo 64 KiB)."); return false;
    }
    return configureData(file.readAll(), urls, error);
}

bool TurnService::configureAutomatic(const AutomaticConfig& options, QString& error) {
    m_secret.clear(); m_urls.clear(); m_config.clear();
    const auto& url = options.serverUrl;
    if (!url.isValid() || url.scheme() != "wss" || url.host().isEmpty()
        || !url.userInfo().isEmpty() || (!url.path().isEmpty() && url.path() != "/")
        || url.hasQuery() || url.hasFragment() || url.port(443) < 1) {
        error = tr("--server-url richiede wss://dominio[:porta], senza credenziali, query o percorsi.");
        return false;
    }
    if (!options.port || !options.minPort || options.minPort > options.maxPort
        || (options.port >= options.minPort && options.port <= options.maxPort)) {
        error = tr("Porte TURN non valide: usa un intervallo relay ordinato che non includa la porta di ascolto.");
        return false;
    }
    // Bound the DNS wait; a failed lookup must never silently produce a relay
    // advertising a private address. Resolve anew in each server process.
    QList<QHostAddress> addresses;
    QHostAddress literal;
    if (literal.setAddress(url.host())) {
        addresses.append(literal);
    } else {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool completed = false;
        const auto lookup = QHostInfo::lookupHost(url.host(), &loop, [&](const QHostInfo& info) {
            completed = true;
            addresses = info.addresses();
            loop.quit();
        });
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(10000);
        loop.exec();
        if (!completed) {
            QHostInfo::abortHostLookup(lookup);
            error = tr("Timeout DNS per il dominio del server; controlla rete e DNS dinamico.");
            return false;
        }
    }
    QList<QHostAddress> ipv4;
    for (const auto& address : addresses) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()
            && !address.isMulticast() && !address.isLinkLocal()
            && address != QHostAddress(QHostAddress::AnyIPv4)
            && address != QHostAddress(QHostAddress::Broadcast) && !ipv4.contains(address))
            ipv4.append(address);
    }
    if (ipv4.size() != 1) {
        error = tr("Il dominio del server deve risolvere a un solo IPv4 unicast (record A). Controlla il DNS o usa --turn-config.");
        return false;
    }
    const auto external = ipv4.first();
    const auto localAddresses = QNetworkInterface::allAddresses();
    auto relay = options.relayAddress;
    if (relay.isNull()) {
        if (localAddresses.contains(external)) {
            relay = external;
        } else {
            // UDP connect selects the source interface via the routing table;
            // no application datagram is sent and no external HTTP service is used.
            QUdpSocket route;
            route.connectToHost(external, options.port);
            if (route.waitForConnected(1000)) relay = route.localAddress();
        }
    }
    if (relay.protocol() != QAbstractSocket::IPv4Protocol || relay.isLoopback()
        || relay.isLinkLocal() || !localAddresses.contains(relay)) {
        error = tr("Impossibile rilevare l'IPv4 locale per TURN; specifica --turn-relay-ip con un indirizzo di questa macchina.");
        return false;
    }
    QByteArray secret;
    for (int i = 0; i < 8; ++i)
        secret += QByteArray::number(QRandomGenerator::system()->generate(), 16).rightJustified(8, '0');
    QByteArray config = "# Generated at signaling startup; private, ephemeral configuration.\n";
    config += "listening-ip=" + relay.toString().toLatin1() + '\n';
    config += "relay-ip=" + relay.toString().toLatin1() + '\n';
    if (external != relay)
        config += "external-ip=" + external.toString().toLatin1() + '/' + relay.toString().toLatin1() + '\n';
    config += "listening-port=" + QByteArray::number(options.port) + '\n';
    config += "min-port=" + QByteArray::number(options.minPort) + '\n';
    config += "max-port=" + QByteArray::number(options.maxPort) + '\n';
    config += "realm=screen-streaming\nfingerprint\nuse-auth-secret\nstatic-auth-secret=" + secret + '\n';
    config += "stale-nonce=600\nuser-quota=12\ntotal-quota=128\nrelay-threads=2\n"
              "no-tcp-relay\nno-tls\ndtls=0\ncli=0\nno-multicast-peers\n"
              "denied-peer-ip=0.0.0.0-0.255.255.255\ndenied-peer-ip=169.254.0.0-169.254.255.255\n";
    QStringList urls;
    for (const auto* transport : {"udp", "tcp"}) {
        QUrl endpoint;
        endpoint.setScheme("turn"); endpoint.setHost(url.host()); endpoint.setPort(options.port);
        endpoint.setQuery(QStringLiteral("transport=") + QLatin1String(transport));
        urls.append(endpoint.toString(QUrl::FullyEncoded));
    }
    return configureData(config, urls, error);
}

bool TurnService::configureData(const QByteArray& config, const QStringList& urls, QString& error) {
    m_secret.clear(); m_urls.clear(); m_config.clear();
    bool auth = false;
    QByteArray secret;
    for (auto line : config.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const auto separator = line.indexOf('=');
        const auto key = (separator < 0 ? line : line.left(separator)).trimmed();
        const auto value = separator < 0 ? QByteArray("1") : line.mid(separator + 1).trimmed();
        // A single, unambiguous auth mode, and a foreground child we can stop.
        if (key == "daemon" || key == "no-auth" || key == "lt-cred-mech" || key == "user"
            || key == "oauth" || key == "rest-api-separator") {
            error = tr("Rimuovi daemon/no-auth/lt-cred-mech/user/oauth/rest-api-separator dalla configurazione coturn gestita.");
            return false;
        }
        if (key == "use-auth-secret") auth = value == "1" || value == "true" || value == "yes" || value == "on";
        if ((key == "dtls" || key == "no-dtls") && value != "1" && value != "true"
            && value != "yes" && value != "on" && value != "0" && value != "false"
            && value != "no" && value != "off") {
            error = tr("dtls/no-dtls richiede un valore booleano: 0/1, false/true, no/yes oppure off/on.");
            return false;
        }
        if (key == "static-auth-secret") {
            if (!secret.isEmpty()) { error = tr("Usa un solo static-auth-secret."); return false; }
            secret = value;
        }
    }
    static const QRegularExpression secretPattern(QStringLiteral("^[a-fA-F0-9]{64}$"));
    if (!auth || !secretPattern.match(QString::fromLatin1(secret)).hasMatch()) {
        error = tr("Coturn richiede use-auth-secret e static-auth-secret con 64 cifre esadecimali (openssl rand -hex 32).");
        return false;
    }
    if (urls.isEmpty() || urls.size() > 8) { error = tr("Specifica da 1 a 8 --turn-url pubblici."); return false; }
    for (const auto& text : urls) {
        const QUrl url(text, QUrl::StrictMode);
        const QUrlQuery query(url);
        const auto items = query.queryItems();
        if (!url.isValid() || (url.scheme() != "turn" && url.scheme() != "turns")
            || url.host().isEmpty() || !url.userInfo().isEmpty() || !url.path().isEmpty()
            || url.hasFragment() || url.port() < 1 || url.port() > 65535
            || (url.hasQuery() && (items.size() != 1 || items.first().first != "transport"
                || (items.first().second != "udp" && items.first().second != "tcp")))
            || (url.scheme() == "turns" && query.queryItemValue("transport") == "udp")) {
            error = tr("TURN URL non valido: usa turn://host:3478?transport=udp o tcp, oppure turns://host:5349?transport=tcp, senza credenziali.");
            return false;
        }
    }
    m_secret = secret; m_config = config; m_urls = urls;
    return true;
}

bool TurnService::start(const QString& executable, QString& error) {
    if (m_secret.isEmpty() || !m_directory.isValid()) { error = tr("Configurazione TURN non disponibile."); return false; }
    // Probe the executable, not its package version: distributions may backport
    // the opt-in CLI/DTLS flags. -n prevents reading an unrelated system config.
    QProcess probe;
    probe.setProcessChannelMode(QProcess::MergedChannels);
    probe.start(executable, {"-n", "--help"});
    if (!probe.waitForStarted(5000)) {
        error = tr("Impossibile avviare coturn: %1. Installa turnserver sulla macchina del signaling.").arg(probe.errorString());
        return false;
    }
    if (!probe.waitForFinished(5000)) {
        probe.kill(); probe.waitForFinished(1000);
        error = tr("Timeout durante il rilevamento delle opzioni coturn."); return false;
    }
    const auto help = QString::fromLocal8Bit(probe.readAll());
    auto supports = [&](const QString& option) {
        return QRegularExpression("(?:^|\\n)[ \\t]*--" + option + "(?:[ \\t=]|$)").match(help).hasMatch();
    };
    const bool modernDtls = supports("dtls"), modernCli = supports("cli");
    if ((!modernDtls && !supports("no-dtls")) || (!modernCli && !supports("no-cli"))) {
        error = tr("Impossibile riconoscere le opzioni CLI/DTLS dell'eseguibile coturn."); return false;
    }
    QByteArray runtimeConfig;
    for (const auto& line : m_config.split('\n')) {
        const auto trimmed = line.trimmed();
        const auto separator = trimmed.indexOf('=');
        const auto key = (separator < 0 ? trimmed : trimmed.left(separator)).trimmed();
        // The managed server always disables coturn's administrative CLI.
        if (key == "cli" || key == "no-cli") continue;
        if (key == "dtls" || key == "no-dtls") {
            const auto value = separator < 0 ? QByteArray("1") : trimmed.mid(separator + 1).trimmed();
            const bool enabled = value == "1" || value == "true" || value == "yes" || value == "on";
            const bool dtls = key == "dtls" ? enabled : !enabled;
            runtimeConfig += modernDtls ? (dtls ? "dtls=1\n" : "dtls=0\n")
                                        : (dtls ? "no-dtls=0\n" : "no-dtls=1\n");
        } else {
            runtimeConfig += line + '\n';
        }
    }
    runtimeConfig += modernCli ? "cli=0\n" : "no-cli\n";
    // Snapshot the validated config. Private directory and file; the shared
    // secret never appears in argv, signaling messages, invitations or settings.
    const auto path = m_directory.filePath("turnserver.conf");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(runtimeConfig) != runtimeConfig.size() || !file.flush()) {
        error = tr("Impossibile preparare la configurazione privata di coturn."); return false;
    }
    file.close();
    m_stopping = false;
    m_process.start(executable, {"-c", path, "--log-file=stdout", "--simple-log",
                                "--userdb=" + m_directory.filePath("turndb"),
                                "--pidfile=" + m_directory.filePath("turnserver.pid")});
    if (!m_process.waitForStarted(5000)) {
        error = tr("Impossibile avviare coturn: %1. Installa turnserver sulla macchina del signaling.").arg(m_process.errorString());
        return false;
    }
    if (m_process.waitForFinished(300)) {
        error = tr("Coturn si è arrestato all’avvio: controlla configurazione, porte e log del server."); return false;
    }
    return true;
}

void TurnService::stop() {
    m_stopping = true;
    if (m_process.state() == QProcess::NotRunning) return;
    m_process.terminate();
    if (!m_process.waitForFinished(3000)) { m_process.kill(); m_process.waitForFinished(3000); }
}

QJsonArray TurnService::credentials(const QString& clientId, qint64 now) const {
    QJsonArray result;
    if (m_secret.isEmpty()) return result;
    const auto username = QByteArray::number(now + CredentialLifetime) + ':' + clientId.toUtf8();
    const auto password = QMessageAuthenticationCode::hash(username, m_secret, QCryptographicHash::Sha1).toBase64();
    // Explicit percent encoding is required by webrtcbin, including ':' in the
    // timestamp:user and '+', '/', '=' in the base64 password.
    const auto userInfo = QString::fromLatin1(QUrl::toPercentEncoding(QString::fromUtf8(username))) + ':'
        + QString::fromLatin1(QUrl::toPercentEncoding(QString::fromLatin1(password))) + '@';
    for (auto url : m_urls) {
        url.insert(url.indexOf("://") + 3, userInfo);
        result.append(url);
    }
    return result;
}

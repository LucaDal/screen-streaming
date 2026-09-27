#include "TurnService.h"
#include <QFile>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
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
    const auto config = file.readAll();
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
    // Snapshot the validated config. Private directory and file; the shared
    // secret never appears in argv, signaling messages, invitations or settings.
    const auto path = m_directory.filePath("turnserver.conf");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(m_config) != m_config.size() || !file.flush()) {
        error = tr("Impossibile preparare la configurazione privata di coturn."); return false;
    }
    file.close();
    m_stopping = false;
    m_process.start(executable, {"-c", path, "--no-cli", "--log-file=stdout", "--simple-log",
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

#include "TurnService.h"
#include <QCoreApplication>
#include <QFile>
#include <QNetworkInterface>
#include <QTemporaryDir>
#include <QUrl>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    // Stand in for old/new coturn binaries to verify the actual private file
    // and process arguments, without requiring two installed coturn versions.
    const auto flavor = qEnvironmentVariable("STREAMING_TEST_COTURN_FLAVOR");
    if (!flavor.isEmpty()) {
        const bool modern = flavor == "modern";
        if (app.arguments().contains("--help")) {
            std::cout << (modern ? " --dtls\tEnable DTLS\n --cli\tEnable CLI\n"
                                 : " --no-dtls\tDisable DTLS\n --no-cli\tDisable CLI\n");
            return 0;
        }
        const auto index = app.arguments().indexOf("-c");
        if (index < 0 || index + 1 >= app.arguments().size()) return 2;
        QFile snapshot(app.arguments().at(index + 1));
        if (!snapshot.open(QIODevice::ReadOnly)) return 3;
        const auto config = snapshot.readAll();
        const bool enabled = qEnvironmentVariable("STREAMING_TEST_DTLS_ENABLED") == "1";
        const QByteArray expected = modern ? (enabled ? "dtls=1\n" : "dtls=0\n")
                                           : (enabled ? "no-dtls=0\n" : "no-dtls=1\n");
        if (!config.contains('\n' + expected) || !config.endsWith(modern ? "cli=0\n" : "no-cli\n")
            || config.contains(modern ? "\nno-dtls" : "\ndtls=") || config.contains("\ncli=1\n")
            || app.arguments().contains("--no-cli")) return 4;
        return app.exec();
    }
    QTemporaryDir directory;
    QFile file(directory.filePath("turn.conf"));
    const QByteArray valid = "use-auth-secret\nstatic-auth-secret=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
    const QStringList urls{"turn://example.com:3478?transport=udp", "turn://example.com:3478?transport=tcp",
                           "turns://example.com:5349?transport=tcp"};
    TurnService turn;
    QString error;
    auto configure = [&](const QByteArray& config, const QStringList& endpoints) {
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        file.write(config); file.close();
        return turn.configure(file.fileName(), endpoints, error);
    };
    auto require = [](bool okay, const char* step) {
        if (!okay) std::cerr << "FAIL: " << step << '\n';
        return okay;
    };
    if (!require(configure(valid, urls), "valid config")) return 1;
    const auto credentials = turn.credentials("alice", 1700000000);
    const QUrl uri(credentials.first().toString());
    if (!require(credentials.size() == 3
        && uri.userName(QUrl::FullyDecoded) == "1700086400:alice"
        && uri.password(QUrl::FullyDecoded) == "eIsEQQ3BwPpu1RAw5aWD59feHqM="
        && credentials.first().toString().contains("1700086400%3Aalice")
        && credentials.first().toString().contains("HqM%3D@")
        && !credentials.first().toString().contains("0123456789abcdef")
        && credentials != turn.credentials("bob", 1700000000)
        && credentials != turn.credentials("alice", 1700000001), "REST HMAC vector, escaping, per-user expiry")) return 1;
    for (const auto& invalid : {QByteArray("no-auth\n"), valid + "daemon\n", valid + "user=a:b\n",
         valid + "use-auth-secret=0\n", valid + "static-auth-secret=another\n", valid + "dtls=maybe\n",
         QByteArray("use-auth-secret\nstatic-auth-secret=short\n")}) {
        if (!require(!configure(invalid, urls) && turn.credentials("alice", 1).isEmpty(), "reject unsafe config and clear credentials")) return 1;
    }
    for (const auto& invalid : {"http://example.com:3478", "turn://user:pass@example.com:3478",
         "turn://example.com", "turn://example.com:3478/path", "turn://example.com:3478?transport=bad",
         "turns://example.com:5349?transport=udp", "turn://example.com:3478?transport=tcp&extra=1"}) {
        if (!require(!configure(valid, {invalid}), "reject malformed URL")) return 1;
    }
    if (!require(!configure(valid, {}), "require public endpoint")) return 1;
    if (!require(configure(valid, urls) && !turn.start(directory.filePath("missing-turnserver"), error), "missing coturn fails")) return 1;
    for (const auto* mode : {"modern", "legacy"}) {
        for (const auto& directive : {QByteArray("no-dtls\n"), QByteArray("dtls=0\n"),
             QByteArray("no-dtls=false\n"), QByteArray("dtls=1\n")}) {
            const bool enabled = directive == "no-dtls=false\n" || directive == "dtls=1\n";
            if (!require(configure(valid + directive + "no-cli\ncli=1\n", urls), "compatibility input")) return 1;
            qputenv("STREAMING_TEST_COTURN_FLAVOR", mode);
            qputenv("STREAMING_TEST_DTLS_ENABLED", enabled ? "1" : "0");
            const bool started = turn.start(QCoreApplication::applicationFilePath(), error);
            qunsetenv("STREAMING_TEST_COTURN_FLAVOR");
            qunsetenv("STREAMING_TEST_DTLS_ENABLED");
            if (!require(started, "old/new coturn flags preserve DTLS intent and disable admin CLI")) return 1;
            turn.stop();
        }
    }
    TurnService::AutomaticConfig automatic;
    for (const auto* invalid : {"ws://example.com:8443", "wss://user:pass@example.com", "wss://example.com/path",
         "wss://example.com?query=1", "wss://example.com#fragment", "wss://example.com:0",
         "wss://127.0.0.1", "wss://[::1]", "wss://0.0.0.0", "wss://224.0.0.1"}) {
        automatic.serverUrl = QUrl(QString::fromLatin1(invalid), QUrl::StrictMode);
        if (!require(!turn.configureAutomatic(automatic, error)
            && turn.credentials("alice", 1).isEmpty(), "reject unsafe automatic endpoint and clear credentials")) return 1;
    }
    automatic.serverUrl = QUrl("wss://203.0.113.10:8443");
    automatic.port = 0;
    if (!require(!turn.configureAutomatic(automatic, error), "reject zero TURN port")) return 1;
    automatic.port = automatic.minPort;
    if (!require(!turn.configureAutomatic(automatic, error), "reject overlapping TURN ports")) return 1;
    automatic.port = 3478;
    automatic.maxPort = automatic.minPort - 1;
    if (!require(!turn.configureAutomatic(automatic, error), "reject inverted relay range")) return 1;
    automatic.maxPort = 49260;
    automatic.relayAddress = QHostAddress("127.0.0.1");
    if (!require(!turn.configureAutomatic(automatic, error), "reject loopback relay override")) return 1;
    for (const auto& local : QNetworkInterface::allAddresses()) {
        if (local.protocol() != QAbstractSocket::IPv4Protocol || local.isLoopback() || local.isLinkLocal()) continue;
        automatic.relayAddress = local;
        if (!require(turn.configureAutomatic(automatic, error), "automatic NAT configuration")) return 1;
        const auto first = turn.credentials("alice", 1700000000);
        const QUrl endpoint(first.first().toString());
        if (!require(first.size() == 2 && endpoint.host() == "203.0.113.10" && endpoint.port() == 3478
            && endpoint.userName(QUrl::FullyDecoded) == "1700086400:alice"
            && !endpoint.password().isEmpty(), "automatic UDP/TCP credentials")) return 1;
        if (!require(turn.configureAutomatic(automatic, error)
            && first != turn.credentials("alice", 1700000000), "fresh secret on regeneration")) return 1;
        automatic.serverUrl = QUrl(QString("wss://%1:8443").arg(local.toString()));
        automatic.relayAddress.clear();
        if (!require(turn.configureAutomatic(automatic, error), "automatic directly attached address")) return 1;
        break;
    }
    std::cout << "PASS: TURN config validation, REST credentials, URI encoding, missing executable\n";
}

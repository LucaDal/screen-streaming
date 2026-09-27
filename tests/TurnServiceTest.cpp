#include "TurnService.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
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
         valid + "use-auth-secret=0\n", valid + "static-auth-secret=another\n",
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
    std::cout << "PASS: TURN config validation, REST credentials, URI encoding, missing executable\n";
}

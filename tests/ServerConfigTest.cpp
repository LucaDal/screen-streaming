#include "ServerConfig.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    const auto path = directory.filePath("signaling.ini");
    auto parser = [] {
        auto result = std::make_unique<QCommandLineParser>();
        result->addOption({{"p", "port"}, "Port", "number", "8443"});
        result->addOption({"insecure-local", "Local mode"});
        for (const auto* name : {"config", "server-url", "token", "token-file", "cert", "key", "turn-config",
             "turn-url", "turn-executable", "turn-relay-ip", "turn-port", "turn-min-port", "turn-max-port"})
            result->addOption({QString::fromLatin1(name), "Value", "value"});
        return result;
    };
    auto write = [&](const QByteArray& config) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(config) == config.size();
    };
    auto require = [](bool okay, const char* step) {
        if (!okay) std::cerr << "FAIL: " << step << '\n';
        return okay;
    };
    QString error;
    const QByteArray config = "# Example configuration\n  # commented-option=value\n; INI comment\n"
        "server-url=wss://stream.example.com:9443\nport=9443\n"
        "token-file=keys/token#part\ncert=fullchain.pem\nkey=privkey.pem\n"
        "turn-executable=bin/turnserver\ninsecure-local=false\n"
        "turn-url=turn://stream.example.com:3478?transport=udp, turn://stream.example.com:3478?transport=tcp\n";
    if (!write(config)) return 1;
    auto options = parser();
    const QStringList args{"server", "--config", path};
    if (!require(options->parse(args) && applyServerConfig(*options, args, error), "load INI")) return 1;
    if (!require(options->value("port") == "9443"
        && options->value("server-url") == "wss://stream.example.com:9443"
        && options->value("token-file") == directory.filePath("keys/token#part")
        && options->value("cert") == directory.filePath("fullchain.pem")
        && options->value("key") == directory.filePath("privkey.pem")
        && options->value("turn-executable") == directory.filePath("bin/turnserver")
        && options->values("turn-url").size() == 2 && !options->isSet("insecure-local"),
        "paths relative to INI directory, repeated URLs and false flag")) return 1;
    options = parser();
    const auto overrides = args + QStringList{"-p", "10443", "--token-file", "cli-token", "--turn-url", "turn://override.example.com:3478"};
    if (!require(options->parse(overrides) && applyServerConfig(*options, overrides, error)
        && options->value("port") == "10443" && options->value("token-file") == "cli-token"
        && options->values("turn-url") == QStringList{"turn://override.example.com:3478"},
        "CLI overrides aliases, paths and entire URL list")) return 1;
    for (const auto& invalid : {QByteArray("typo=true\n"), QByteArray("[turn]\nport=3478\n"),
         QByteArray("config=recursive.ini\n"), QByteArray("insecure-local=maybe\n"), QByteArray("token-file=\n"),
         QByteArray("[unterminated\nport=8443\n")}) {
        options = parser();
        if (!write(invalid) || !require(options->parse(args) && !applyServerConfig(*options, args, error), "reject malformed configuration")) return 1;
    }
    options = parser();
    const QStringList missing{"server", "--config", directory.filePath("missing.ini")};
    if (!require(options->parse(missing) && !applyServerConfig(*options, missing, error), "missing INI fails")) return 1;
    options = parser();
    if (!write("insecure-local=true\nturn-executable=turnserver\n")) return 1;
    if (!require(options->parse(args) && applyServerConfig(*options, args, error)
        && options->isSet("insecure-local") && options->value("turn-executable") == "turnserver",
        "true flag and executable from PATH")) return 1;
    options = parser();
    const auto duplicate = args + QStringList{"--config", path};
    if (!require(options->parse(duplicate) && !applyServerConfig(*options, duplicate, error), "reject multiple config files")) return 1;
    options = parser();
    const QStringList legacy{"server", "-p", "9443"};
    if (!require(options->parse(legacy) && applyServerConfig(*options, legacy, error)
        && options->value("port") == "9443", "legacy CLI remains supported")) return 1;
    const QString testToken = "test-secret-with-#-hash-and=equals";
    if (!write("# Direct token\ntoken=" + testToken.toUtf8() + '\n')) return 1;
    options = parser();
    if (!require(options->parse(args) && applyServerConfig(*options, args, error)
        && options->value("token") == testToken && !options->isSet("token-file"),
        "direct token preserves hash and equals")) return 1;
    options = parser();
    const auto fileOverride = args + QStringList{"--token-file", "cli-token"};
    if (!require(options->parse(fileOverride) && applyServerConfig(*options, fileOverride, error)
        && !options->isSet("token") && options->value("token-file") == "cli-token",
        "CLI token file overrides INI direct token")) return 1;
    if (!write("token-file=token\n")) return 1;
    options = parser();
    const auto tokenOverride = args + QStringList{"--token", testToken};
    if (!require(options->parse(tokenOverride) && applyServerConfig(*options, tokenOverride, error)
        && !options->isSet("token-file") && options->value("token") == testToken,
        "CLI direct token overrides INI token file")) return 1;
    if (app.arguments().size() > 1) {
        options = parser();
        const QStringList example{"server", "--config", app.arguments().at(1)};
        if (!require(options->parse(example) && applyServerConfig(*options, example, error)
            && options->value("server-url") == "wss://stream.example.com:8443"
            && !options->isSet("turn-config"), "shipped example loads with commented options")) return 1;
    }
    std::cout << "PASS: signaling INI, relative paths, CLI precedence, repeated URLs, booleans and validation\n";
}

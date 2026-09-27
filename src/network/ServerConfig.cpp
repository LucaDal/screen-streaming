#include "ServerConfig.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSet>
#include <QTemporaryFile>

bool applyServerConfig(QCommandLineParser& parser, const QStringList& arguments, QString& error) {
    if (!parser.isSet("config")) return true;
    if (parser.values("config").size() != 1) {
        error = QStringLiteral("Specifica un solo --config."); return false;
    }
    const QFileInfo location(parser.value("config"));
    QFile file(location.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) {
        error = QStringLiteral("Impossibile leggere il file di configurazione signaling (massimo 64 KiB)."); return false;
    }
    // QSettings recognizes ';' comments, but not the common '#' spelling
    // used by our example. Normalize full comment lines, preserving '#' in
    // values and keeping the original location for relative path resolution.
    QByteArray normalized;
    for (auto line : file.readAll().split('\n')) {
        if (line.trimmed().startsWith('#')) line = ";";
        normalized += line + '\n';
    }
    file.close();
    QTemporaryFile snapshot;
    if (!snapshot.open() || snapshot.write(normalized) != normalized.size() || !snapshot.flush()) {
        error = QStringLiteral("Impossibile preparare la configurazione signaling temporanea."); return false;
    }
    QSettings settings(snapshot.fileName(), QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    const auto keys = settings.allKeys();
    if (settings.status() != QSettings::NoError) {
        error = QStringLiteral("File di configurazione signaling INI non valido."); return false;
    }
    const QSet<QString> allowed{"port", "token", "token-file", "cert", "key", "server-url", "turn-config",
        "turn-url", "turn-executable", "turn-relay-ip", "turn-port", "turn-min-port", "turn-max-port", "insecure-local"};
    const QSet<QString> paths{"token-file", "cert", "key", "turn-config"};
    QStringList merged{arguments.first()};
    for (const auto& key : keys) {
        if (!allowed.contains(key)) {
            error = QStringLiteral("Opzione sconosciuta nel file signaling: %1. Usa chiavi senza sezioni, come server-url e token-file.").arg(key);
            return false;
        }
        if (parser.isSet(key)) continue;
        // Either CLI spelling overrides the other token source in the INI.
        if ((key == "token" && parser.isSet("token-file"))
            || (key == "token-file" && parser.isSet("token"))) continue;
        const auto value = settings.value(key);
        if (key == "insecure-local") {
            const auto boolean = value.toString().trimmed().toLower();
            if (boolean == "true" || boolean == "1") merged.append("--insecure-local");
            else if (boolean != "false" && boolean != "0") {
                error = QStringLiteral("insecure-local nel file INI deve essere true o false."); return false;
            }
            continue;
        }
        const auto values = key == "turn-url" ? value.toStringList() : QStringList{value.toString()};
        if (values.isEmpty()) { error = QStringLiteral("Valore vuoto per %1.").arg(key); return false; }
        for (auto text : values) {
            text = text.trimmed();
            if (text.isEmpty()) { error = QStringLiteral("Valore vuoto per %1.").arg(key); return false; }
            if (paths.contains(key) || (key == "turn-executable" && (text.contains('/') || text.contains('\\'))))
                text = QDir::cleanPath(location.absoluteDir().absoluteFilePath(text));
            merged.append("--" + key + '=' + text);
        }
    }
    merged.append(arguments.mid(1));
    if (!parser.parse(merged)) { error = parser.errorText(); return false; }
    return true;
}

#pragma once
#include <QJsonArray>
#include <QHostAddress>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>

// Coturn implements TURN; the signaling process owns its lifetime and issues
// short-lived REST credentials only to authenticated room participants.
class TurnService final : public QObject
{
    Q_OBJECT
public:
    struct AutomaticConfig {
        QUrl serverUrl;
        QHostAddress relayAddress; // Optional override for hosts with several interfaces.
        quint16 port = 3478;
        quint16 minPort = 49160;
        quint16 maxPort = 49260;
    };
    explicit TurnService(QObject* parent = nullptr);
    ~TurnService() override;
    bool configure(const QString& configFile, const QStringList& urls, QString& error);
    bool configureAutomatic(const AutomaticConfig& options, QString& error);
    bool start(const QString& executable, QString& error);
    void stop();
    QJsonArray credentials(const QString& clientId, qint64 now) const;
    static constexpr qint64 CredentialLifetime = 24 * 60 * 60;
signals:
    void failed();
private:
    bool configureData(const QByteArray& config, const QStringList& urls, QString& error);
    QProcess m_process;
    QTemporaryDir m_directory;
    QByteArray m_config, m_secret;
    QStringList m_urls;
    bool m_stopping = false;
};

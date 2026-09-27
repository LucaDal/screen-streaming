#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QObject>
#include <QVariantMap>

// Qt's PipeWire capture backend can stop the stream without closing its
// XDG portal session. Track this app's ScreenCast session on the same bus
// and explicitly close it. No private Qt API or desktop-wide cleanup.
// StreamingApp creates only one portal session at a time (screen capture).
class PortalSessionGuard final : public QObject
{
    Q_OBJECT
public:
    explicit PortalSessionGuard(QObject* parent = nullptr,
                                QDBusConnection bus = QDBusConnection::sessionBus());
    ~PortalSessionGuard() override;
    bool begin(QString& error);
    void creationFailed();
    void stop();

signals:
    void cancelled();

private slots:
    void response(uint result, const QVariantMap& values, const QDBusMessage& message);
    void closed(const QVariantMap& details, const QDBusMessage& message);

private:
    void closeSession(const QString& path);
    QDBusConnection m_bus;
    QString m_requestPrefix;
    QString m_sessionPrefix;
    QString m_session;
    bool m_subscribed = false;
    bool m_active = false;
    bool m_waitingForSession = false;
};

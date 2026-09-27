#include "PortalSessionGuard.h"
#include <QDBusObjectPath>
#include <utility>

namespace {
const QString service = QStringLiteral("org.freedesktop.portal.Desktop");
const QString requestInterface = QStringLiteral("org.freedesktop.portal.Request");
const QString sessionInterface = QStringLiteral("org.freedesktop.portal.Session");
}

PortalSessionGuard::PortalSessionGuard(QObject* parent, QDBusConnection bus)
    : QObject(parent), m_bus(std::move(bus))
{
    QString sender = m_bus.baseService();
    sender.remove(0, 1);
    sender.replace('.', '_');
    m_requestPrefix = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/").arg(sender);
    m_sessionPrefix = QStringLiteral("/org/freedesktop/portal/desktop/session/%1/").arg(sender);
    const bool responses = m_bus.connect(service, {}, requestInterface, QStringLiteral("Response"),
        this, SLOT(response(uint,QVariantMap,QDBusMessage)));
    const bool closures = m_bus.connect(service, {}, sessionInterface, QStringLiteral("Closed"),
        this, SLOT(closed(QVariantMap,QDBusMessage)));
    m_subscribed = m_bus.isConnected() && responses && closures;
}

PortalSessionGuard::~PortalSessionGuard() { stop(); }

bool PortalSessionGuard::begin(QString& error)
{
    error.clear();
    if (!m_subscribed) {
        error = tr("Impossibile controllare la sessione del portale desktop: D-Bus non disponibile.");
        return false;
    }
    if (m_waitingForSession || m_active) {
        error = tr("Il portale sta completando la richiesta precedente. Riprova tra un momento.");
        return false;
    }
    m_active = true;
    m_waitingForSession = true;
    return true;
}

void PortalSessionGuard::closeSession(const QString& path)
{
    auto message = QDBusMessage::createMethodCall(service, path, sessionInterface, QStringLiteral("Close"));
    // Send even during destruction, without a watcher whose lifetime could
    // cancel cleanup. Closing an already closed session is harmless.
    m_bus.call(message, QDBus::NoBlock);
}

void PortalSessionGuard::stop()
{
    m_active = false;
    if (!m_session.isEmpty()) closeSession(std::exchange(m_session, {}));
    // Keep listening for CreateSession's late response: stop can precede it.
    // Until then begin() prevents a new generation from consuming that reply.
}

void PortalSessionGuard::creationFailed()
{
    // Qt also reports synchronous CreateSession method errors, for which
    // the portal will never emit a Response signal.
    if (m_active && m_waitingForSession) {
        m_waitingForSession = false;
        m_active = false;
    }
}

void PortalSessionGuard::response(uint result, const QVariantMap& values, const QDBusMessage& message)
{
    if (!message.path().startsWith(m_requestPrefix)) return;
    if (m_waitingForSession) {
        if (result != 0) {
            m_waitingForSession = false;
            if (m_active) { m_active = false; emit cancelled(); }
            return;
        }
        QString session = values.value(QStringLiteral("session_handle")).toString();
        if (session.isEmpty()) session = values.value(QStringLiteral("session_handle")).value<QDBusObjectPath>().path();
        if (!session.startsWith(m_sessionPrefix)) return;
        m_waitingForSession = false;
        if (!m_active || result != 0) {
            closeSession(session);
            if (m_active) { m_active = false; emit cancelled(); }
            return;
        }
        m_session = session;
    } else if (m_active && result != 0 && !m_session.isEmpty()) {
        // The Qt backend uses the session token for SelectSources and Start.
        // Ignore cancellations of unrelated requests (e.g. file dialogs).
        if (message.path() != m_requestPrefix + m_session.section('/', -1)) return;
        stop();
        emit cancelled();
    }
}

void PortalSessionGuard::closed(const QVariantMap&, const QDBusMessage& message)
{
    if (!m_active || message.path() != m_session) return;
    m_session.clear();
    m_active = false;
    emit cancelled();
}

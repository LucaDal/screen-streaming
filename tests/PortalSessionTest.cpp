#include "PortalSessionGuard.h"
#include <QCoreApplication>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QThread>
#include <QSet>
#include <iostream>
#include <memory>

class FakePortal final : public QDBusVirtualObject {
public:
    QSet<QString> openSessions;
    QStringList closed;
    QString introspect(const QString&) const override { return {}; }
    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override {
        if (message.interface() != "org.freedesktop.portal.Session" || message.member() != "Close") return false;
        closed.append(message.path());
        openSessions.remove(message.path());
        connection.send(message.createReply());
        return true;
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto client = QDBusConnection::sessionBus();
    auto service = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-portal");
    if (!client.isConnected() || !service.registerService("org.freedesktop.portal.Desktop")) {
        std::cerr << "Run this test inside dbus-run-session, never on the desktop bus\n"; return 1;
    }
    FakePortal portal;
    if (!service.registerVirtualObject("/org/freedesktop/portal/desktop", &portal, QDBusConnection::SubPath)) return 1;
    QString sender = client.baseService().mid(1); sender.replace('.', '_');
    const QString requestPrefix = "/org/freedesktop/portal/desktop/request/" + sender + '/';
    const QString sessionPrefix = "/org/freedesktop/portal/desktop/session/" + sender + '/';
    auto response = [&](uint result, const QString& path, const QString& session = QString()) {
        QVariantMap data;
        if (!session.isEmpty()) { data["session_handle"] = session; portal.openSessions.insert(session); }
        auto event = QDBusMessage::createTargetedSignal(client.baseService(), path, "org.freedesktop.portal.Request", "Response");
        event << result << data;
        service.send(event);
    };
    auto pump = [&](auto done) {
        QElapsedTimer timer; timer.start();
        while (!done() && timer.elapsed() < 2000) { app.processEvents(); QThread::msleep(1); }
        return done();
    };
    auto settle = [&] {
        QElapsedTimer timer; timer.start();
        pump([&] { return timer.elapsed() > 30; });
    };
    auto require = [](bool ok, const char* message) {
        if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
    };
    QString error;
    auto guard = std::make_unique<PortalSessionGuard>();
    int cancelled = 0;
    QObject::connect(guard.get(), &PortalSessionGuard::cancelled, &app, [&] { ++cancelled; });
    require(guard->begin(error), "synchronous failure setup");
    guard->creationFailed(); guard->stop();
    for (int attempt = 0; attempt < 12; ++attempt) {
        require(guard->begin(error), "restart rejected");
        const auto token = QString("session%1").arg(attempt);
        const auto session = sessionPrefix + token;
        response(0, requestPrefix + "create", session); settle();
        require(portal.openSessions.contains(session), "session closed during capture");
        guard->stop(); guard->stop();
        require(pump([&] { return !portal.openSessions.contains(session); }), "stop did not close portal session");
        require(portal.closed.count(session) == 1, "duplicate Close");
    }
    // Responses owned by a different D-Bus client must not be adopted.
    require(guard->begin(error), "foreign response setup");
    const QString foreign = "/org/freedesktop/portal/desktop/session/9_999/foreign";
    response(0, "/org/freedesktop/portal/desktop/request/9_999/create", foreign); settle();
    response(0, requestPrefix + "create", sessionPrefix + "own"); settle();
    guard->stop();
    require(pump([&] { return portal.closed.contains(sessionPrefix + "own"); }), "owned session not closed");
    require(portal.openSessions.contains(foreign) && !portal.closed.contains(foreign), "closed another application's session");
    portal.openSessions.remove(foreign);
    // Stop before CreateSession replies; a new generation must not steal it.
    require(guard->begin(error), "late response setup");
    guard->stop();
    require(!guard->begin(error), "overlapping portal creation allowed");
    response(0, requestPrefix + "create", sessionPrefix + "late");
    require(pump([&] { return portal.closed.contains(sessionPrefix + "late"); }), "late session leaked");
    require(guard->begin(error), "restart after late reply");
    response(0, requestPrefix + "create", sessionPrefix + "cancel"); settle();
    // An unrelated file-picker cancellation must not stop screen sharing.
    response(1, requestPrefix + "filepicker"); settle();
    require(cancelled == 0, "unrelated request stopped screen sharing");
    response(1, requestPrefix + "cancel");
    require(pump([&] { return cancelled == 1 && portal.closed.contains(sessionPrefix + "cancel"); }), "picker cancellation leaked session");
    require(guard->begin(error), "create failure setup");
    response(2, requestPrefix + "create");
    require(pump([&] { return cancelled == 2; }), "create error not handled");
    require(guard->begin(error), "restart after create failure");
    response(0, requestPrefix + "create", sessionPrefix + "external"); settle();
    portal.openSessions.remove(sessionPrefix + "external");
    auto closed = QDBusMessage::createTargetedSignal(client.baseService(), sessionPrefix + "external", "org.freedesktop.portal.Session", "Closed");
    closed << QVariantMap{}; service.send(closed);
    require(pump([&] { return cancelled == 3; }), "desktop stop not handled");
    require(guard->begin(error), "destruction setup");
    response(0, requestPrefix + "create", sessionPrefix + "destruction"); settle();
    guard.reset();
    require(pump([&] { return portal.openSessions.isEmpty(); }), "destruction leaked session");
    std::cout << "PASS: repeated start/stop, delayed creation, cancellation, failure, desktop stop and destruction close only owned sessions\n";
}

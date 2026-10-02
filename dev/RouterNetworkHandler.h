#ifndef ROUTERNETWORKHANDLER_H
#define ROUTERNETWORKHANDLER_H

#include <QObject>
#include <QStringList>
#include <QHostAddress>
#include <QElapsedTimer>
#include "../RouterInterfaceStat.h"

class QUdpSocket;

// Listens for the router's RAW per-interface metrics (UDP id 60, rows
// 600/601/602) on ROUTER_TELEMETRY_UDP_PORT and republishes them as signals.
//
// The wire shape is the contract shared with XBFirm's telemetry port:
//   byte 0..1 = datagram id, "60"
//   byte 2    = ConnType (row kind)
//   byte 3..  = payload, '#'-separated
// See RouterInterfaceStat.h for the field layout and ConnType.
//
// This handler does NOT decide anything. It parses and emits; whatever acts on
// the numbers is deliberately elsewhere, so the parse can be tested and the
// policy changed without touching the socket.
class RouterNetworkHandler : public QObject
{
    Q_OBJECT
public:
    explicit RouterNetworkHandler(QObject *parent = nullptr);
    ~RouterNetworkHandler();

    void start(quint16 port);
    void stop();

signals:
    // One VIDEO or C2 row. The router emits one per interface per connType, so
    // a tick with N interfaces produces up to 2N of these, then one routerTick.
    void onRouterRow(const RouterInterfaceStat &row);

    // The PRIMARY row (602) — the chosen interface's name, or "-".
    //
    // Doubles as the end-of-tick marker: the router sends 602 LAST within a
    // tick (TelemetryModule.cpp in the router repo — the per-interface rows must
    // arrive before the receiver can act on the primary's name). A consumer
    // wanting a consistent snapshot should accumulate onRouterRow and act here,
    // rather than on each row.
    void onRouterTick(const QString &primaryInterface);

private slots:
    void handleUdpReadyRead();

private:
    void parseDatagram(const QByteArray &data, const QHostAddress &sender, quint16 senderPort);
    void emitRow(ConnType connType, const QStringList &fields);
    void logRowFields(ConnType connType, const QStringList &fields);

    QUdpSocket *m_udpSocket;

    // Throttles the "this was the router's 550/560 stub" line. Without it the
    // stub would print twice a second forever and bury the id-60 rows it sits
    // beside. Invalid on first use, which is why the check tests isValid().
    QElapsedTimer _lastStubLog;
};

#endif // ROUTERNETWORKHANDLER_H

#ifndef ROUTERNETWORKHANDLER_H
#define ROUTERNETWORKHANDLER_H

#include <QObject>
#include <QStringList>
#include <QHostAddress>

class QUdpSocket;

// Row kind of a router telemetry datagram (UDP id 60, the 3rd byte).
//
// Local copy of the enum XBFirm declares in settings/GeneralConfigs.h. Kept
// local on purpose: XBQoSService is a separate binary and must not include
// across the xbfirm repo. The wire values are the contract; if the router ever
// reaches XBQoSService directly on port 12346 these two definitions must stay
// numerically identical.
//
//   VIDEO / C2   -> 14 '#'-separated fields:
//       [0] interfaceName  [1] priority      [2] isActivated (0/1)
//       [3] isConnected    [4] isMptcp       [5] usable (0/1)
//       [6] score          [7] trend (0..4)  [8] throughput kbps
//       [9] lossRate %     [10] rtt ms       [11] minRtt ms
//       [12] queuingDelay ms  [13] jitter ms
//   PRIMARY      -> ONE field: the chosen primary interface name, or "-".
enum class ConnType : int {
    VIDEO   = 0,   // 600
    C2      = 1,   // 601
    PRIMARY = 2    // 602
};

class RouterNetworkHandler : public QObject
{
    Q_OBJECT
public:
    explicit RouterNetworkHandler(QObject *parent = nullptr);
    ~RouterNetworkHandler();

    void start(quint16 port);
    void stop();

private slots:
    void handleUdpReadyRead();

private:
    void parseDatagram(const QByteArray &data, const QHostAddress &sender, quint16 senderPort);
    void logRowFields(ConnType connType, const QStringList &fields);
    void logPrimary(const QStringList &fields);

    QUdpSocket *m_udpSocket;
};

#endif // ROUTERNETWORKHANDLER_H

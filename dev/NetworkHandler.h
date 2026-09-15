#ifndef NETWORKHANDLER_H
#define NETWORKHANDLER_H

#include <QObject>
#include <QUdpSocket>
#include "../SRTPeerStat.h"

class NetworkHandler : public QObject
{
    Q_OBJECT
public:
    explicit NetworkHandler(QObject *parent = nullptr);
    ~NetworkHandler();

<<<<<<< HEAD
    void start(quint16 port = SRT_ABR_QOS_UDP_PORT);
    void stop();
    bool isConnected() const { return m_isConnected; }
    quint16 listenPort() const { return m_listenPort; }
=======
    void start();
    void stop();
>>>>>>> 0b68b3da79ddedf29a5064388cb35446afa0ede8

signals:
    void onQosDataReceived(const QVector<SRTPeerStat> &peers);
    void onC2DataReceived(const QVector<SRTPeerStat> &peers);

private slots:
<<<<<<< HEAD
    void checkTimeoutWatchdog();
=======
>>>>>>> 0b68b3da79ddedf29a5064388cb35446afa0ede8
    void handleUdpReadyRead();

private:
    SRTPeerStat parsePeerEntry(const QString &entry);
    QUdpSocket *m_udpSocket;
<<<<<<< HEAD
    QTimer *m_watchdogTimer;
    quint16 m_listenPort;
    bool m_isConnected;
    qint64 m_lastPacketTime;
    int m_packetCount;
=======
>>>>>>> 0b68b3da79ddedf29a5064388cb35446afa0ede8
};

#endif // NETWORKHANDLER_H

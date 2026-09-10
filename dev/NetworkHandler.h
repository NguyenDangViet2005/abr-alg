#ifndef NETWORKHANDLER_H
#define NETWORKHANDLER_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantList>
#include <QVariantMap>
#include <QDateTime>
#include <QDebug>
#include "../ABRConfigs.h"

class NetworkHandler : public QObject
{
    Q_OBJECT
public:
    explicit NetworkHandler(QObject *parent = nullptr);
    ~NetworkHandler();

    void start(quint16 port = SRT_ABR_QOS_UDP_PORT);
    void stop();
    bool isConnected() const { return m_isConnected; }
    quint16 listenPort() const { return m_listenPort; }

signals:
    void onQosDataReceived(const QVariantList &clients);
    void onC2DataReceived(const QVariantMap &c2Stats);
    void onConnectionStateChanged(bool isConnected);

private slots:
    void checkTimeoutWatchdog();
    void handleUdpReadyRead();

private:
    QUdpSocket *m_udpSocket;
    QTimer *m_watchdogTimer;
    quint16 m_listenPort;
    bool m_isConnected;
    qint64 m_lastPacketTime;
    int m_packetCount;
};

#endif // NETWORKHANDLER_H

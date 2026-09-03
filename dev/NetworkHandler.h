#ifndef NETWORKHANDLER_H
#define NETWORKHANDLER_H

#include <QObject>
#include <QUdpSocket>
<<<<<<< HEAD
#include <QTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantList>
#include <QVariantMap>
#include <QDateTime>
#include <QDebug>
#include "../ABRConfigs.h"
=======
#include <QByteArray>
>>>>>>> 0680d627399d948513f1c74c97a9c9c2e8e346e5

class NetworkHandler : public QObject
{
    Q_OBJECT
public:
    explicit NetworkHandler(QObject *parent = nullptr);
    ~NetworkHandler();

<<<<<<< HEAD
    void start(const QString &host = QOS_SERVER_DEFAULT_HOST, quint16 port = QOS_SERVER_DEFAULT_PORT, int intervalMs = QOS_SERVER_POLL_INTERVAL_MS);
    void stop();
    void setServerAddress(const QString &host, quint16 port);
    void setPollInterval(int intervalMs);
    bool isConnected() const { return m_isConnected; }

signals:
    void onQosDataReceived(const QVariantList &clients);
    void onC2DataReceived(const QVariantMap &c2Stats);
    void onConnectionStateChanged(bool isConnected);

private slots:
    void sendQosQuery();
    void handleUdpReadyRead();

private:
    QUdpSocket *m_udpSocket;
    QTimer *m_pollTimer;
    QHostAddress m_serverHost;
    quint16 m_serverPort;
    bool m_isConnected;
    qint64 m_lastPacketTime;
    int m_packetCount;
=======
    // Port UDP lắng nghe (trùng với SRT QoS listener bên ngoài).
    static constexpr quint16 UDP_LISTEN_PORT = 12345;

    void start();
    void stop();

signals:
    // Phát mỗi khi nhận được 1 datagram UDP từ port 12345.
    void dataReceived(const QByteArray &datagram);

private slots:
    void handleReadyRead();

private:
    QUdpSocket* m_udpSocket = nullptr;
>>>>>>> 0680d627399d948513f1c74c97a9c9c2e8e346e5
};

#endif // NETWORKHANDLER_H

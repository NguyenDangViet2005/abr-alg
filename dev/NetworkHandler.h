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

    void start(const QString &host = QOS_SERVER_DEFAULT_HOST, quint16 port = QOS_SERVER_DEFAULT_PORT, int intervalMs = QOS_SERVER_POLL_INTERVAL_MS);
    void stop();
    void setServerAddress(const QString &host, quint16 port);
    void setPollInterval(int intervalMs);
    bool isConnected() const { return m_isConnected; }

signals:
    void onQosDataReceived(const QVariantList &clients);
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
};

#endif // NETWORKHANDLER_H

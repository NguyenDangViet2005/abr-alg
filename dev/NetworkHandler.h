#ifndef NETWORKHANDLER_H
#define NETWORKHANDLER_H

#include <QObject>
#include <QUdpSocket>
#include "../SRTPeerStat.h"

#include "../ABRConfigs.h"

class NetworkHandler : public QObject
{
    Q_OBJECT
public:
    explicit NetworkHandler(QObject *parent = nullptr);
    ~NetworkHandler();

    void start(quint16 port = SRT_ABR_QOS_UDP_PORT);
    void stop();

signals:
    void onQosDataReceived(const QVector<SRTPeerStat> &peers);
    void onC2DataReceived(const QVector<SRTPeerStat> &peers);

private slots:
    void handleUdpReadyRead();

private:
    void parseJsonDatagram(const QByteArray &data, const QHostAddress &senderAddress, quint16 senderPort);
    void parsePrefixDatagram(const QByteArray &data, const QHostAddress &senderAddress, quint16 senderPort);
    SRTPeerStat parsePeerEntry(const QString &entry);
    QUdpSocket *m_udpSocket;
};

#endif // NETWORKHANDLER_H
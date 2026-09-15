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

    void start();
    void stop();

signals:
    void onQosDataReceived(const QVector<SRTPeerStat> &peers);
    void onC2DataReceived(const QVector<SRTPeerStat> &peers);

private slots:
    void handleUdpReadyRead();

private:
    SRTPeerStat parsePeerEntry(const QString &entry);
    QUdpSocket *m_udpSocket;
};

#endif // NETWORKHANDLER_H

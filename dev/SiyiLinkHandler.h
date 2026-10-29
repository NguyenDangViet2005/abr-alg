#ifndef SIYILINKHANDLER_H
#define SIYILINKHANDLER_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include "../SiyiLinkStat.h"
#include "../ABRConfigs.h"

class SiyiLinkHandler : public QObject
{
    Q_OBJECT
public:
    explicit SiyiLinkHandler(QObject *parent = nullptr);
    ~SiyiLinkHandler();

    void start(const QString &auIp = SIYI_AU_DEFAULT_IP,
               quint16 auPort = SIYI_AU_DEFAULT_PORT,
               quint16 localPort = SIYI_LOCAL_DEFAULT_PORT);
    void stop();

    const SiyiLinkStat& currentStat() const { return m_lastStat; }

signals:
    void onSiyiLinkUpdated(const SiyiLinkStat &stat);
    void onSiyiLinkLost();

private slots:
    void handleUdpReadyRead();
    void sendPollRequest();
    void checkLinkWatchdog();

private:
    bool parseResponse(const QByteArray &data, SiyiLinkStat &outStat);

    QUdpSocket *m_udpSocket;
    QTimer *m_pollTimer;
    QTimer *m_watchdogTimer;

    QString m_auIp;
    quint16 m_auPort;
    quint16 m_localPort;

    SiyiLinkStat m_lastStat;
    qint64 m_lastResponseTime;
    bool m_isLinkAlive;
};

#endif // SIYILINKHANDLER_H

#include "NetworkHandler.h"
#include <QNetworkDatagram>
#include <QDebug>

static const int SRT_DEBUG_PORT = 12345;
static const int STREAM_PREFIX_LEN = 4;

NetworkHandler::NetworkHandler(QObject *parent)
    : QObject(parent)
    , m_udpSocket(new QUdpSocket(this))
{
    connect(m_udpSocket, &QUdpSocket::readyRead, this, &NetworkHandler::handleUdpReadyRead);
}

NetworkHandler::~NetworkHandler()
{
    stop();
}

void NetworkHandler::start()
{
    if (m_udpSocket->state() != QAbstractSocket::BoundState) {
        m_udpSocket->bind(QHostAddress::Any, SRT_DEBUG_PORT);
    }
    qInfo() << "[NetworkHandler] Listening UDP 0.0.0.0:" << SRT_DEBUG_PORT;
}

void NetworkHandler::stop()
{
    if (m_udpSocket && m_udpSocket->state() == QAbstractSocket::BoundState) {
        m_udpSocket->close();
    }
    qInfo() << "[NetworkHandler] Stopped.";
}

void NetworkHandler::handleUdpReadyRead()
{
    while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = m_udpSocket->receiveDatagram();
        QByteArray data = datagram.data();
        if (data.size() < STREAM_PREFIX_LEN) continue;

        QByteArray prefix = data.mid(0, STREAM_PREFIX_LEN);
        QByteArray body = data.mid(STREAM_PREFIX_LEN);

        QString streamType = (prefix == "9990") ? "Camera" :
                             (prefix == "9991") ? "Controlling" : "Unknown";

        qInfo().noquote() << QString("[NetworkHandler] UDP recv from %1:%2 (%3 bytes) | prefix=%4 [%5]")
                                 .arg(datagram.senderAddress().toString())
                                 .arg(datagram.senderPort())
                                 .arg(data.size())
                                 .arg(QString::fromLatin1(prefix))
                                 .arg(streamType);

        QVector<SRTPeerStat> peers;
        const QList<QByteArray> entries = body.split('#');
        for (const QByteArray &entry : entries) {
            if (entry.isEmpty()) continue;
            peers.append(parsePeerEntry(QString::fromLatin1(entry)));
        }

        if (peers.isEmpty()) continue;

        for (int i = 0; i < peers.size(); ++i) {
            const SRTPeerStat &p = peers[i];
            qInfo().noquote() << QString("[NetworkHandler]   Peer[%1] %2:%3 | RTT: %4ms | SendRate: %5 Mbps | BW: %6 Mbps | Loss: %7/%8 | Retrans: %9")
                                     .arg(i)
                                     .arg(p.peerAddress)
                                     .arg(p.peerPort)
                                     .arg(p.msRTT, 0, 'f', 2)
                                     .arg(p.mbpsSendRate, 0, 'f', 2)
                                     .arg(p.mbpsBandwidth, 0, 'f', 2)
                                     .arg(p.pktSndLossTotal)
                                     .arg(p.pktRcvLossTotal)
                                     .arg(p.pktRetransTotal);
        }

        emit onQosDataReceived(peers);

        if (prefix == "9991") {
            emit onC2DataReceived(peers);
        }
    }
}

SRTPeerStat NetworkHandler::parsePeerEntry(const QString &entry)
{
    SRTPeerStat stat;
    const QStringList pairs = entry.split(';', Qt::SkipEmptyParts);
    for (const QString &pair : pairs) {
        const int eqIdx = pair.indexOf('=');
        if (eqIdx < 0) continue;

        const QString key = pair.left(eqIdx);
        const QString value = pair.mid(eqIdx + 1);

        if (key == "peerAddress")          stat.peerAddress = value;
        else if (key == "peerPort")        stat.peerPort = value.toInt();
        else if (key == "msTimeStamp")     stat.msTimeStamp = value.toLongLong();
        else if (key == "pktSentTotal")    stat.pktSentTotal = value.toLongLong();
        else if (key == "pktRecvTotal")    stat.pktRecvTotal = value.toLongLong();
        else if (key == "pktSndLossTotal") stat.pktSndLossTotal = value.toInt();
        else if (key == "pktRcvLossTotal") stat.pktRcvLossTotal = value.toInt();
        else if (key == "pktRetransTotal") stat.pktRetransTotal = value.toInt();
        else if (key == "byteSentTotal")   stat.byteSentTotal = value.toLongLong();
        else if (key == "byteRecvTotal")   stat.byteRecvTotal = value.toLongLong();
        else if (key == "mbpsSendRate")    stat.mbpsSendRate = value.toDouble();
        else if (key == "mbpsRecvRate")    stat.mbpsRecvRate = value.toDouble();
        else if (key == "msRTT")           stat.msRTT = value.toDouble();
        else if (key == "mbpsBandwidth")   stat.mbpsBandwidth = value.toDouble();
        else if (key == "pktSndDropTotal") stat.pktSndDropTotal = value.toInt();
        else if (key == "pktRcvDropTotal") stat.pktRcvDropTotal = value.toInt();
    }
    return stat;
}

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
    // Positional format: addr;port;ts;pktSent;pktRecv;lossSnd;lossRcv;retrans;byteSent;byteRecv;rateSend;rateRecv;rtt;bw;dropSnd;dropRcv
    const QStringList f = entry.split(';', Qt::SkipEmptyParts);
    if (f.size() < 16) {
        qWarning() << "[NetworkHandler] Expected 16 fields, got" << f.size() << ":" << entry;
        return {};
    }

    SRTPeerStat stat;
    stat.peerAddress      = f[0];
    stat.peerPort         = f[1].toInt();
    stat.msTimeStamp      = f[2].toLongLong();
    stat.pktSentTotal     = f[3].toLongLong();
    stat.pktRecvTotal     = f[4].toLongLong();
    stat.pktSndLossTotal  = f[5].toInt();
    stat.pktRcvLossTotal  = f[6].toInt();
    stat.pktRetransTotal  = f[7].toInt();
    stat.byteSentTotal    = f[8].toLongLong();
    stat.byteRecvTotal    = f[9].toLongLong();
    stat.mbpsSendRate     = f[10].toDouble();
    stat.mbpsRecvRate     = f[11].toDouble();
    stat.msRTT            = f[12].toDouble();
    stat.mbpsBandwidth    = f[13].toDouble();
    stat.pktSndDropTotal  = f[14].toInt();
    stat.pktRcvDropTotal  = f[15].toInt();
    return stat;
}

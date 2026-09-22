#include "NetworkHandler.h"
#include <QNetworkDatagram>
#include <QDebug>

// Giao thức UDP production từ SRT daemon trên xblink:
//   prefix "9990" = QoS camera metrics, "9991" = C2 telemetry4
//   entry         = addr;port;ts;pktSent;pktRecv;lossSnd;lossRcv;retrans;
//                   byteSent;byteRecv;rateSend;rateRecv;rtt;bw;dropSnd;dropRcv
static const int STREAM_PREFIX_LEN = 4;
static const char QOS_CAMERA_PREFIX[] = "9990";
static const char QOS_C2_PREFIX[] = "9991";

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

void NetworkHandler::start(quint16 port)
{
    if (m_udpSocket->state() != QAbstractSocket::BoundState) {
        m_udpSocket->bind(QHostAddress::Any, port);
    }
    qInfo() << "[NetworkHandler] Listening UDP 0.0.0.0:" << port;
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
        const QByteArray data = m_udpSocket->receiveDatagram().data();
        if (!data.isEmpty()) {
            parsePrefixDatagram(data);
        }
    }
}

void NetworkHandler::parsePrefixDatagram(const QByteArray &data)
{
    if (data.size() < STREAM_PREFIX_LEN) return;

    const QByteArray prefix = data.left(STREAM_PREFIX_LEN);
    const bool isCamera = (prefix == QOS_CAMERA_PREFIX);
    if (!isCamera && prefix != QOS_C2_PREFIX) return;

    QVector<SRTPeerStat> peers;
    const QList<QByteArray> entries = data.mid(STREAM_PREFIX_LEN).split('#');
    for (const QByteArray &entry : entries) {
        if (entry.isEmpty()) continue;
        peers.append(parsePeerEntry(QString::fromLatin1(entry)));
    }

    if (peers.isEmpty()) return;

    if (isCamera) {
        emit onQosDataReceived(peers);
    } else {
        emit onC2DataReceived(peers);
    }
}

SRTPeerStat NetworkHandler::parsePeerEntry(const QString &entry)
{
    // Positional: addr;port;ts;pktSent;pktRecv;lossSnd;lossRcv;retrans;byteSent;byteRecv;rateSend;rateRecv;rtt;bw;dropSnd;dropRcv
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

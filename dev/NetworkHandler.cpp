#include "NetworkHandler.h"
#include <QNetworkDatagram>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

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
        QNetworkDatagram datagram = m_udpSocket->receiveDatagram();
        QByteArray data = datagram.data();
        if (data.isEmpty()) continue;

        QByteArray trimmed = data.trimmed();
        if (trimmed.startsWith('{')) {
            parseJsonDatagram(data, datagram.senderAddress(), datagram.senderPort());
        } else {
            parsePrefixDatagram(data, datagram.senderAddress(), datagram.senderPort());
        }
    }
}

void NetworkHandler::parseJsonDatagram(const QByteArray &data, const QHostAddress &senderAddress, quint16 senderPort)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return;
    }

    QJsonObject root = doc.object();
    QString src = root.value("source").toString();
    if (src.isEmpty()) {
        src = QString("%1:%2").arg(senderAddress.toString()).arg(senderPort);
    }

    // 1. Camera metrics
    QJsonObject metrics = root.value("metrics").toObject();
    if (!metrics.isEmpty()) {
        SRTPeerStat cam;
        cam.peerAddress = src;
        cam.peerPort = senderPort;
        cam.msRTT = metrics.value("rtt_ms").toDouble();
        cam.mbpsBandwidth = metrics.value("estimated_bandwidth_mbps").toDouble();
        if (cam.mbpsBandwidth <= 0.0) {
            cam.mbpsBandwidth = metrics.value("bandwidth_mbps").toDouble();
        }
        cam.mbpsSendRate = metrics.value("send_rate_mbps").toDouble();
        cam.pktSndLossTotal = metrics.value("total_packets_lost").toInt();
        cam.pktSndDropTotal = metrics.value("flight_size").toInt();
        cam.pktRetransTotal = metrics.value("recv_buffer_ms").toInt();

        QVector<SRTPeerStat> camPeers;
        camPeers.append(cam);
        emit onQosDataReceived(camPeers);
    }

    // 2. C2 Telemetry metrics
    if (root.contains("c2_metrics")) {
        QJsonObject c2Obj = root.value("c2_metrics").toObject();
        SRTPeerStat c2;
        c2.peerAddress = src;
        c2.peerPort = senderPort;
        c2.msRTT = c2Obj.value("rtt_ms").toDouble();
        c2.mbpsBandwidth = c2Obj.value("delivery_rate_mbps").toDouble();
        c2.pktRetransTotal = c2Obj.value("retransmits").toInt();
        c2.pktSndLossTotal = c2Obj.value("tcpi_loss").toInt();
        c2.pktSndDropTotal = c2Obj.value("unacked_pkts").toInt();
        c2.isC2Only = c2Obj.value("c2_only").toBool() || (c2Obj.value("congestion_state").toString().toUpper() == "C2_ONLY");

        QVector<SRTPeerStat> c2Peers;
        c2Peers.append(c2);
        emit onC2DataReceived(c2Peers);
    }
}

void NetworkHandler::parsePrefixDatagram(const QByteArray &data, const QHostAddress &senderAddress, quint16 senderPort)
{
    Q_UNUSED(senderAddress);
    Q_UNUSED(senderPort);
    if (data.size() < STREAM_PREFIX_LEN) return;

    QByteArray prefix = data.mid(0, STREAM_PREFIX_LEN);
    QByteArray body = data.mid(STREAM_PREFIX_LEN);

    QVector<SRTPeerStat> peers;
    const QList<QByteArray> entries = body.split('#');
    for (const QByteArray &entry : entries) {
        if (entry.isEmpty()) continue;
        peers.append(parsePeerEntry(QString::fromLatin1(entry)));
    }

    if (peers.isEmpty()) return;

    if (prefix == "9990") {
        emit onQosDataReceived(peers);
    } else if (prefix == "9991") {
        emit onC2DataReceived(peers);
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

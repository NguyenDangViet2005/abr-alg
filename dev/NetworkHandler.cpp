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
        else if (key == "isC2Only" || key == "c2_only") stat.isC2Only = (value == "1" || value.toLower() == "true");
    }
    return stat;
}

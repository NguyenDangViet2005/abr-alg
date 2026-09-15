#include "NetworkHandler.h"
#include <QNetworkDatagram>
#include <QDebug>

static const int SRT_DEBUG_PORT = 12345;
static const int STREAM_PREFIX_LEN = 4;

NetworkHandler::NetworkHandler(QObject *parent)
    : QObject(parent)
<<<<<<< HEAD
    , m_udpSocket(nullptr)
    , m_watchdogTimer(nullptr)
    , m_listenPort(SRT_ABR_QOS_UDP_PORT)
    , m_isConnected(false)
    , m_lastPacketTime(0)
    , m_packetCount(0)
{
    m_udpSocket = new QUdpSocket(this);
    m_watchdogTimer = new QTimer(this);

    connect(m_udpSocket, &QUdpSocket::readyRead, this, &NetworkHandler::handleUdpReadyRead);
    connect(m_watchdogTimer, &QTimer::timeout, this, &NetworkHandler::checkTimeoutWatchdog);
=======
    , m_udpSocket(new QUdpSocket(this))
{
    connect(m_udpSocket, &QUdpSocket::readyRead, this, &NetworkHandler::handleUdpReadyRead);
>>>>>>> 0b68b3da79ddedf29a5064388cb35446afa0ede8
}

NetworkHandler::~NetworkHandler()
{
    stop();
}

<<<<<<< HEAD
void NetworkHandler::start(quint16 port)
{
    m_listenPort = port;

    if (m_udpSocket->state() != QAbstractSocket::BoundState) {
        if (!m_udpSocket->bind(QHostAddress::Any, m_listenPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            qCritical() << "[NetworkHandler] FAILED to bind UDP Server on port" << m_listenPort << ":" << m_udpSocket->errorString();
        } else {
            qInfo() << "[NetworkHandler] UDP QoS Server is LISTENING on port" << m_listenPort
                    << "(Ready for incoming QoS datagrams from Clients/GCS)";
        }
    }

    // Chạy watchdog timer kiểm tra timeout định kỳ (mỗi 1000ms)
    m_watchdogTimer->start(1000);
=======
void NetworkHandler::start()
{
    if (m_udpSocket->state() != QAbstractSocket::BoundState) {
        m_udpSocket->bind(QHostAddress::Any, SRT_DEBUG_PORT);
    }
    qInfo() << "[NetworkHandler] Listening UDP 0.0.0.0:" << SRT_DEBUG_PORT;
>>>>>>> 0b68b3da79ddedf29a5064388cb35446afa0ede8
}

void NetworkHandler::stop()
{
<<<<<<< HEAD
    if (m_watchdogTimer && m_watchdogTimer->isActive()) {
        m_watchdogTimer->stop();
    }
    if (m_udpSocket && m_udpSocket->state() == QAbstractSocket::BoundState) {
        m_udpSocket->close();
    }
    m_isConnected = false;
    qInfo() << "[NetworkHandler] Stopped UDP QoS Server.";
}

void NetworkHandler::checkTimeoutWatchdog()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_isConnected && (now - m_lastPacketTime) > 3500) {
        m_isConnected = false;
        qWarning() << "[NetworkHandler] Client QoS stream TIMEOUT. Waiting for packets...";
        emit onConnectionStateChanged(false);
    }
=======
    if (m_udpSocket && m_udpSocket->state() == QAbstractSocket::BoundState) {
        m_udpSocket->close();
    }
    qInfo() << "[NetworkHandler] Stopped.";
>>>>>>> 0b68b3da79ddedf29a5064388cb35446afa0ede8
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

<<<<<<< HEAD
        QJsonObject root = doc.object();
        QJsonObject metrics;
        if (root.contains("metrics") && root.value("metrics").isObject()) {
            metrics = root.value("metrics").toObject();
        } else {
            metrics = root;
        }

        // Hỗ trợ linh hoạt cả key lồng trong metrics lẫn key trực tiếp ở root
        double rtt = 0.0;
        if (metrics.contains("rtt_ms")) rtt = metrics.value("rtt_ms").toDouble();
        else if (metrics.contains("msRTT")) rtt = metrics.value("msRTT").toDouble();
        else if (metrics.contains("rtt")) rtt = metrics.value("rtt").toDouble();

        double bw = 0.0;
        if (metrics.contains("estimated_bandwidth_mbps")) bw = metrics.value("estimated_bandwidth_mbps").toDouble();
        else if (metrics.contains("bandwidth_mbps")) bw = metrics.value("bandwidth_mbps").toDouble();
        else if (metrics.contains("mbpsBandwidth")) bw = metrics.value("mbpsBandwidth").toDouble();
        else if (metrics.contains("bw")) bw = metrics.value("bw").toDouble();
        else if (metrics.contains("bandwidth_kbps")) bw = metrics.value("bandwidth_kbps").toDouble() / 1000.0;
        else if (metrics.contains("bitrate_kbps")) bw = metrics.value("bitrate_kbps").toDouble() / 1000.0;
        else if (metrics.contains("bitrate")) {
            double rawBitrate = metrics.value("bitrate").toDouble();
            // Nếu bitrate > 100 thì đơn vị là kbps, nếu nhỏ hơn thì là Mbps
            bw = (rawBitrate > 100.0) ? (rawBitrate / 1000.0) : rawBitrate;
        }

        double sendRate = 0.0;
        if (metrics.contains("send_rate_mbps")) sendRate = metrics.value("send_rate_mbps").toDouble();
        else if (metrics.contains("mbpsSendRate")) sendRate = metrics.value("mbpsSendRate").toDouble();
        else if (metrics.contains("send_rate_kbps")) sendRate = metrics.value("send_rate_kbps").toDouble() / 1000.0;

        int loss = 0;
        if (metrics.contains("total_packets_lost")) loss = metrics.value("total_packets_lost").toInt();
        else if (metrics.contains("pktSndLossTotal")) loss = metrics.value("pktSndLossTotal").toInt();
        else if (metrics.contains("loss")) loss = metrics.value("loss").toInt();

        int flight = 0;
        if (metrics.contains("flight_size")) flight = metrics.value("flight_size").toInt();
        else if (metrics.contains("pktFlightSize")) flight = metrics.value("pktFlightSize").toInt();

        int bufMs = 0;
        if (metrics.contains("recv_buffer_ms")) bufMs = metrics.value("recv_buffer_ms").toInt();
        else if (metrics.contains("pktSndBuf")) bufMs = metrics.value("pktSndBuf").toInt();

        QString src = root.value("stream_source").toString();
        if (src.isEmpty()) {
            src = QString("%1:%2").arg(datagram.senderAddress().toString()).arg(datagram.senderPort());
        }

        // Đóng gói cấu trúc SRT Camera stats chuẩn cho ABRFactory & BelaCoder
        QVariantMap clientQos;
        clientQos["peerAddress"] = src;
        clientQos["msRTT"] = rtt;
        clientQos["mbpsBandwidth"] = bw;
        clientQos["mbpsSendRate"] = sendRate;
        clientQos["pktSndLossTotal"] = loss;
        clientQos["pktFlightSize"] = flight;
        clientQos["pktSndBuf"] = bufMs;

        QVariantList clientList;
        clientList.append(clientQos);

        m_packetCount++;
        m_lastPacketTime = QDateTime::currentMSecsSinceEpoch();

        if (!m_isConnected) {
            m_isConnected = true;
            qInfo().noquote() << QString(">>> [NetworkHandler] FIRST PACKET RECEIVED from %1! Raw Bandwidth: %2 Mbps (%3 kbps), RTT: %4ms, Loss: %5 <<<")
                        .arg(src)
                        .arg(bw, 0, 'f', 2)
                        .arg(bw * 1000.0, 0, 'f', 0)
                        .arg(rtt, 0, 'f', 1)
                        .arg(loss);
            emit onConnectionStateChanged(true);
        }

        // Bắn dữ liệu QoS vào ABR engine
        emit onQosDataReceived(clientList);

        // Đóng gói cấu trúc C2 Telemetry stats (TCP_INFO) nếu có trong JSON
        if (root.contains("c2_metrics")) {
            QJsonObject c2Obj = root.value("c2_metrics").toObject();
            QVariantMap c2Stats;
            c2Stats["rtt_ms"] = c2Obj.value("rtt_ms").toDouble();
            c2Stats["rtt_var_ms"] = c2Obj.value("rtt_var_ms").toDouble();
            c2Stats["delivery_rate_mbps"] = c2Obj.value("delivery_rate_mbps").toDouble();
            c2Stats["retransmits"] = c2Obj.value("retransmits").toInt();
            c2Stats["tcpi_loss"] = c2Obj.value("tcpi_loss").toInt();
            c2Stats["unacked_pkts"] = c2Obj.value("unacked_pkts").toInt();
            c2Stats["snd_cwnd"] = c2Obj.value("snd_cwnd").toInt();
            c2Stats["min_rtt_ms"] = c2Obj.value("min_rtt_ms").toDouble();
            c2Stats["congestion_state"] = c2Obj.value("congestion_state").toString();

            emit onC2DataReceived(c2Stats);
=======
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
>>>>>>> 0b68b3da79ddedf29a5064388cb35446afa0ede8
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

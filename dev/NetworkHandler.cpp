#include "NetworkHandler.h"
#include <QNetworkDatagram>
#include <QDebug>

NetworkHandler::NetworkHandler(QObject *parent)
    : QObject(parent)
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
}

NetworkHandler::~NetworkHandler()
{
    stop();
}

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
}

void NetworkHandler::stop()
{
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
}

void NetworkHandler::handleUdpReadyRead()
{
    while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = m_udpSocket->receiveDatagram();
        QByteArray data = datagram.data();
        if (data.isEmpty()) continue;

        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }

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

        double sendRate = 0.0;
        if (metrics.contains("send_rate_mbps")) sendRate = metrics.value("send_rate_mbps").toDouble();
        else if (metrics.contains("mbpsSendRate")) sendRate = metrics.value("mbpsSendRate").toDouble();

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
            qInfo() << "[NetworkHandler] Client connected! Receiving QoS stats from" << src;
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
        }
    }
}

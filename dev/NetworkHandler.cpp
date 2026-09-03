#include "NetworkHandler.h"
#include <QNetworkDatagram>

#include <QDebug>

NetworkHandler::NetworkHandler(QObject *parent)
<<<<<<< HEAD
    : QObject(parent)
    , m_udpSocket(nullptr)
    , m_pollTimer(nullptr)
    , m_serverHost(QOS_SERVER_DEFAULT_HOST)
    , m_serverPort(QOS_SERVER_DEFAULT_PORT)
    , m_isConnected(false)
    , m_lastPacketTime(0)
    , m_packetCount(0)
{
    m_udpSocket = new QUdpSocket(this);
    m_pollTimer = new QTimer(this);

    connect(m_udpSocket, &QUdpSocket::readyRead, this, &NetworkHandler::handleUdpReadyRead);
    connect(m_pollTimer, &QTimer::timeout, this, &NetworkHandler::sendQosQuery);
}
=======
    : QObject{parent}
{}
>>>>>>> 0680d627399d948513f1c74c97a9c9c2e8e346e5

NetworkHandler::~NetworkHandler()
{
    stop();
}

<<<<<<< HEAD
void NetworkHandler::start(const QString &host, quint16 port, int intervalMs)
{
    m_serverHost = QHostAddress(host);
    m_serverPort = port;

    // Bind socket on any free local port to receive UDP responses
    if (m_udpSocket->state() != QAbstractSocket::BoundState) {
        m_udpSocket->bind(QHostAddress::Any, 0);
    }

    m_pollTimer->setInterval(intervalMs > 0 ? intervalMs : QOS_SERVER_POLL_INTERVAL_MS);
    m_pollTimer->start();

    qInfo() << "[NetworkHandler] Started polling QoS Server at" << host << ":" << port
            << "(Interval:" << m_pollTimer->interval() << "ms)";

    // Send immediate first query
    sendQosQuery();
=======
void NetworkHandler::start()
{
    if (m_udpSocket == nullptr) {
        m_udpSocket = new QUdpSocket(this);
        connect(m_udpSocket, &QUdpSocket::readyRead,
                this, &NetworkHandler::handleReadyRead);
        if (!m_udpSocket->bind(QHostAddress::Any, UDP_LISTEN_PORT)) {
            qDebug() << "[NetworkHandler] bind UDP port" << UDP_LISTEN_PORT
                     << "thất bại:" << m_udpSocket->errorString();
        } else {
            qDebug() << "[NetworkHandler] đang lắng nghe UDP trên port" << UDP_LISTEN_PORT;
        }
    }
>>>>>>> 0680d627399d948513f1c74c97a9c9c2e8e346e5
}

void NetworkHandler::stop()
{
<<<<<<< HEAD
    if (m_pollTimer && m_pollTimer->isActive()) {
        m_pollTimer->stop();
    }
    if (m_udpSocket && m_udpSocket->state() == QAbstractSocket::BoundState) {
        m_udpSocket->close();
    }
    m_isConnected = false;
    qInfo() << "[NetworkHandler] Stopped QoS Server polling.";
}

void NetworkHandler::setServerAddress(const QString &host, quint16 port)
{
    m_serverHost = QHostAddress(host);
    m_serverPort = port;
    qInfo() << "[NetworkHandler] Updated target QoS Server address:" << host << ":" << port;
}

void NetworkHandler::setPollInterval(int intervalMs)
{
    if (m_pollTimer && intervalMs > 0) {
        m_pollTimer->setInterval(intervalMs);
        qInfo() << "[NetworkHandler] Updated QoS poll interval:" << intervalMs << "ms";
    }
}

void NetworkHandler::sendQosQuery()
{
    if (!m_udpSocket) return;

    // Check connection timeout (no response for > 3.5 seconds)
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_isConnected && (now - m_lastPacketTime) > 3500) {
        m_isConnected = false;
        qWarning() << "[NetworkHandler] QoS Server response TIMEOUT. Waiting for server...";
        emit onConnectionStateChanged(false);
    }

    // Gửi bản tin query qua UDP tới QoS Server (Port 12345)
    QByteArray queryPayload = "{\"cmd\":\"get_qos\"}";
    m_udpSocket->writeDatagram(queryPayload, m_serverHost, m_serverPort);
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
        if (root.value("status").toString() != "OK") {
            continue;
        }

        QJsonObject metrics = root.value("metrics").toObject();
        double rtt = metrics.value("rtt_ms").toDouble();
        double bw = metrics.value("estimated_bandwidth_mbps").toDouble();
        if (bw <= 0.0) {
            bw = metrics.value("bandwidth_mbps").toDouble();
        }
        double sendRate = metrics.value("send_rate_mbps").toDouble();
        int loss = metrics.value("total_packets_lost").toInt();
        int flight = metrics.value("flight_size").toInt();
        int bufMs = metrics.value("recv_buffer_ms").toInt();
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
            qInfo() << "[NetworkHandler] Connected to QoS Server successfully! Stream Status:"
                    << root.value("stream_state").toString() << "from" << src;
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
=======
    if (m_udpSocket) {
        m_udpSocket->close();
        m_udpSocket->deleteLater();
        m_udpSocket = nullptr;
    }
}

void NetworkHandler::handleReadyRead()
{
    while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(int(m_udpSocket->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        m_udpSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        qDebug() << "[NetworkHandler] nhận datagram" << datagram.size()
                 << "bytes từ" << sender.toString() << ":" << senderPort;
        emit dataReceived(datagram);
>>>>>>> 0680d627399d948513f1c74c97a9c9c2e8e346e5
    }
}

#include "RouterNetworkHandler.h"
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QDebug>

// Wire shape (shared with XBFirm's telemetry port):
//   byte 0..1 = datagram id, "60" for the router's per-interface metrics
//   byte 2    = ConnType (row kind), see enum in the header
//   byte 3..  = payload, '#'-separated
static const int DATAGRAM_ID_LEN = 2;
static const char ROUTER_ID[] = "60";

// VIDEO / C2 rows carry exactly this many fields, positionally:
static const int ROW_FIELD_COUNT = 14;
static const char *const ROW_FIELD_NAMES[ROW_FIELD_COUNT] = {
    "interfaceName", "priority",    "isActivated", "isConnected",
    "isMptcp",       "usable",      "score",       "trend",
    "throughputKbps", "lossRatePct", "rttMs",      "minRttMs",
    "queuingDelayMs", "jitterMs"
};

RouterNetworkHandler::RouterNetworkHandler(QObject *parent)
    : QObject(parent)
    , m_udpSocket(new QUdpSocket(this))
{
    connect(m_udpSocket, &QUdpSocket::readyRead, this, &RouterNetworkHandler::handleUdpReadyRead);
}

RouterNetworkHandler::~RouterNetworkHandler()
{
    stop();
}

void RouterNetworkHandler::start(quint16 port)
{
    if (m_udpSocket->state() == QAbstractSocket::BoundState) {
        return;
    }
    if (!m_udpSocket->bind(QHostAddress::Any, port)) {
        qWarning().nospace() << "[RouterNetworkHandler] bind failed on 0.0.0.0:" << port
                             << " : " << m_udpSocket->errorString();
        return;
    }
    qInfo().nospace() << "[RouterNetworkHandler] Listening UDP 0.0.0.0:" << port;
}

void RouterNetworkHandler::stop()
{
    if (m_udpSocket && m_udpSocket->state() == QAbstractSocket::BoundState) {
        m_udpSocket->close();
    }
    qInfo() << "[RouterNetworkHandler] Stopped.";
}

void RouterNetworkHandler::handleUdpReadyRead()
{
    while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_udpSocket->receiveDatagram();
        parseDatagram(dg.data(), dg.senderAddress(), dg.senderPort());
    }
}

void RouterNetworkHandler::parseDatagram(const QByteArray &data, const QHostAddress &sender, quint16 senderPort)
{
    qDebug() << "--> [RouterNetworkHandler] parseDatagram() called with data size=" << data.size()
             << " from " << sender.toString() << ":" << senderPort;
    const int minLen = DATAGRAM_ID_LEN + 1;   // id + connType byte
    if (data.size() < minLen) {
        qWarning().nospace() << "[ROUTER][RX] datagram too short (" << data.size()
                             << " bytes, need >= " << minLen << ") from "
                             << sender.toString() << ":" << senderPort
                             << " raw=" << QString::fromLatin1(data);
        return;
    }

    const QByteArray id = data.left(DATAGRAM_ID_LEN);
    if (id != ROUTER_ID) {
        // The port is dedicated to router telemetry; any other id means a
        // misdirected sender and is logged rather than silently dropped.
        qDebug().nospace() << "[ROUTER][RX] ignored id=" << QString::fromLatin1(id)
                           << " from " << sender.toString() << ":" << senderPort;
        return;
    }

    const int connTypeRaw = data.mid(DATAGRAM_ID_LEN, 1).toInt();
    const QString payload = QString::fromUtf8(data.mid(DATAGRAM_ID_LEN + 1)).trimmed();
    const QStringList fields = payload.split('#');

    if (connTypeRaw < 0 || connTypeRaw > 2) {
        // A connType outside the three defined values means the router and this
        // parser have diverged. Logged distinctly but parsing continues so the
        // raw payload is visible to diagnose the mismatch.
        qWarning().nospace() << "[ROUTER][RX] id=60 UNKNOWN connType=" << connTypeRaw
                             << " raw=" << payload;
    }
    const ConnType connType = static_cast<ConnType>(connTypeRaw);

    // The sender is logged because the telemetry port takes datagrams from
    // anyone on the segment: a second source would otherwise be
    // indistinguishable from the router's own packets.
    qDebug().nospace() << "[ROUTER][RX] id=60 connType=" << connTypeRaw
                       << " fields=" << fields.size()
                       << " from=" << sender.toString() << ":" << senderPort
                       << " raw=" << payload;

    if (connType == ConnType::PRIMARY) {
        logPrimary(fields);
    } else {
        logRowFields(connType, fields);
    }
}

void RouterNetworkHandler::logRowFields(ConnType connType, const QStringList &fields)
{
    if (fields.size() < ROW_FIELD_COUNT) {
        qWarning().nospace() << "[ROUTER][RX] connType=" << static_cast<int>(connType)
                             << " expected " << ROW_FIELD_COUNT << " fields, got " << fields.size();
    }

    const int count = qMin(fields.size(), ROW_FIELD_COUNT);
    for (int i = 0; i < count; ++i) {
        qDebug().nospace() << "[ROUTER][RX]   " << ROW_FIELD_NAMES[i] << "=" << fields.at(i);
    }
}

void RouterNetworkHandler::logPrimary(const QStringList &fields)
{
    if (fields.size() != 1) {
        qWarning().nospace() << "[ROUTER][RX] PRIMARY expected 1 field, got " << fields.size();
    }
    qDebug().nospace() << "[ROUTER][RX]   primaryInterface=" << fields.value(0, "-");
}

#include "SiyiLinkHandler.h"
#include <QNetworkDatagram>
#include <QDateTime>
#include <QDebug>

// SIYI SDK Request Packet (10 bytes):
// CMD: 0x44 (Get RF Link status / Signal / RSSI / Bandwidth)
static const char SIYI_REQUEST_BYTES[] = {
    '\x55', '\x66', '\x01', '\x00', '\x00',
    '\x00', '\x00', '\x44', '\x05', '\xDC'
};
static const int SIYI_RESPONSE_LENGTH = 46;

static inline int32_t readInt32LE(const unsigned char *p)
{
    uint32_t value = static_cast<uint32_t>(p[0]) |
                     (static_cast<uint32_t>(p[1]) << 8) |
                     (static_cast<uint32_t>(p[2]) << 16) |
                     (static_cast<uint32_t>(p[3]) << 24);
    return static_cast<int32_t>(value);
}

SiyiLinkHandler::SiyiLinkHandler(QObject *parent)
    : QObject(parent)
    , m_udpSocket(new QUdpSocket(this))
    , m_pollTimer(new QTimer(this))
    , m_watchdogTimer(new QTimer(this))
    , m_auIp(SIYI_AU_DEFAULT_IP)
    , m_auPort(SIYI_AU_DEFAULT_PORT)
    , m_localPort(SIYI_LOCAL_DEFAULT_PORT)
    , m_lastResponseTime(0)
    , m_isLinkAlive(false)
{
    connect(m_udpSocket, &QUdpSocket::readyRead, this, &SiyiLinkHandler::handleUdpReadyRead);
    connect(m_pollTimer, &QTimer::timeout, this, &SiyiLinkHandler::sendPollRequest);
    connect(m_watchdogTimer, &QTimer::timeout, this, &SiyiLinkHandler::checkLinkWatchdog);
}

SiyiLinkHandler::~SiyiLinkHandler()
{
    stop();
}

void SiyiLinkHandler::start(const QString &auIp, quint16 auPort, quint16 localPort)
{
    m_auIp = auIp;
    m_auPort = auPort;
    m_localPort = localPort;

    if (m_udpSocket->state() != QAbstractSocket::BoundState) {
        bool bound = m_udpSocket->bind(QHostAddress::AnyIPv4, m_localPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
        if (!bound) {
            qWarning() << "[SiyiLinkHandler] Failed to bind local port" << m_localPort
                       << "- error:" << m_udpSocket->errorString();
        } else {
            qInfo() << "[SiyiLinkHandler] Bound local UDP port" << m_localPort
                    << "-> Target AU:" << m_auIp << ":" << m_auPort;
        }
    }

    m_pollTimer->start(SIYI_POLL_INTERVAL_MS);
    m_watchdogTimer->start(1000);

    // Gửi ngay 1 gói request đầu tiên
    sendPollRequest();
}

void SiyiLinkHandler::stop()
{
    if (m_pollTimer && m_pollTimer->isActive()) {
        m_pollTimer->stop();
    }
    if (m_watchdogTimer && m_watchdogTimer->isActive()) {
        m_watchdogTimer->stop();
    }
    if (m_udpSocket && m_udpSocket->state() == QAbstractSocket::BoundState) {
        m_udpSocket->close();
    }
    m_isLinkAlive = false;
    qInfo() << "[SiyiLinkHandler] Stopped.";
}

void SiyiLinkHandler::sendPollRequest()
{
    if (!m_udpSocket) return;

    qint64 sent = m_udpSocket->writeDatagram(
        SIYI_REQUEST_BYTES,
        sizeof(SIYI_REQUEST_BYTES),
        QHostAddress(m_auIp),
        m_auPort
    );

    if (sent < 0) {
        qWarning() << "[SiyiLinkHandler] Failed to send poll request:" << m_udpSocket->errorString();
    }
}

void SiyiLinkHandler::handleUdpReadyRead()
{
    while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = m_udpSocket->receiveDatagram();
        const QByteArray data = datagram.data();
        if (data.isEmpty()) continue;

        SiyiLinkStat stat;
        if (parseResponse(data, stat)) {
            m_lastResponseTime = QDateTime::currentMSecsSinceEpoch();
            m_lastStat = stat;
            if (!m_isLinkAlive) {
                m_isLinkAlive = true;
                qInfo().noquote() << QString("[SiyiLinkHandler] 🟢 SIYI HM30 Link Established! RSSI: %1 dBm | Signal: %2%")
                                            .arg(stat.rssi).arg(stat.signal);
            }
            emit onSiyiLinkUpdated(m_lastStat);
        }
    }
}

bool SiyiLinkHandler::parseResponse(const QByteArray &data, SiyiLinkStat &outStat)
{
    if (data.size() != SIYI_RESPONSE_LENGTH) {
        return false;
    }

    const unsigned char *buf = reinterpret_cast<const unsigned char*>(data.constData());

    // Header check: 0x55 0x66 0x02 0x24 ... CMD 0x44
    if (buf[0] != 0x55 || buf[1] != 0x66 || buf[2] != 0x02 || buf[3] != 0x24 || buf[7] != 0x44) {
        return false;
    }

    outStat.sequence     = static_cast<uint16_t>(buf[4]) | (static_cast<uint16_t>(buf[5]) << 8);
    outStat.signal       = readInt32LE(&buf[8]);
    outStat.inactiveTime = readInt32LE(&buf[12]);
    outStat.upstream     = readInt32LE(&buf[16]);
    outStat.downstream   = readInt32LE(&buf[20]);
    outStat.txBandwidth  = readInt32LE(&buf[24]);
    outStat.rxBandwidth  = readInt32LE(&buf[28]);
    outStat.rssi         = readInt32LE(&buf[32]);
    outStat.frequency    = readInt32LE(&buf[36]);
    outStat.channel      = readInt32LE(&buf[40]);
    outStat.timestampMs  = QDateTime::currentMSecsSinceEpoch();
    outStat.isAlive      = true;

    return true;
}

void SiyiLinkHandler::checkLinkWatchdog()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_isLinkAlive && (now - m_lastResponseTime > SIYI_LINK_TIMEOUT_MS)) {
        m_isLinkAlive = false;
        m_lastStat.isAlive = false;
        qWarning().noquote() << QString("[SiyiLinkHandler] 🔴 LOST SIYI RF Telemetry (> %1ms). Pausing ABR until valid RSSI returns.")
                                    .arg(SIYI_LINK_TIMEOUT_MS);
        emit onSiyiLinkLost();
    }
}

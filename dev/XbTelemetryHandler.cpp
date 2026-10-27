#include "XbTelemetryHandler.h"
#include <QSocketNotifier>
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QNetworkInterface>
#include <cerrno>
#include <cstring>
#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/filter.h>
#include <unistd.h>
#endif

XbTelemetryHandler::XbTelemetryHandler(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(C2_TELEMETRY_INTERVAL_MS);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &XbTelemetryHandler::publish);
}
XbTelemetryHandler::~XbTelemetryHandler() { stop(); }

bool XbTelemetryHandler::start(const QString &interface, quint16 port)
{
    stop();
    m_debug = qgetenv("XB_C2_DEBUG") == "1";
    const QByteArray filterOverride = qgetenv("XB_C2_BPF");
    m_useKernelFilter = filterOverride.isEmpty() ? C2_TELEMETRY_USE_BPF : filterOverride == "1";
    m_readPackets = 0;
    m_interface = interface;
    m_lastError.clear();
    m_clock.start();
    const QString mode = qEnvironmentVariable("XB_C2_MODE", "real").trimmed().toLower();
    const QString configuredSource = qEnvironmentVariable("XB_C2_SOURCE_IP", C2_TELEMETRY_SOURCE_IP).trimmed();
    // Real C2 data must originate at a local IP of the capture interface.
    // Test C2 originates at the explicitly selected external simulator IP.
    const QString sourceIp = mode == "test" ? configuredSource : QString();
    const QHostAddress sourceAddress(sourceIp);
    QStringList realSourceIps;
    if (mode == "real") {
        const auto captureInterface = QNetworkInterface::interfaceFromName(interface);
        for (const auto &entry : captureInterface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
                realSourceIps.append(entry.ip().toString());
        }
    }
    m_decoder.reset(new C2TelemetryDecoder(port, C2_TELEMETRY_FC_SYSTEM_ID,
        C2_TELEMETRY_FC_COMPONENT_ID, C2_TELEMETRY_STALE_MS,
        sourceIp.isEmpty() ? QString() : sourceAddress.toString(), realSourceIps));
    m_timer.start(); // Keep emitting unavailable samples if capture cannot start.
    if (mode != "real" && mode != "test") {
        m_lastError = "C2 capture setup failed: XB_C2_MODE must be real or test";
        emit captureError(m_lastError);
        return false;
    }
    if (mode == "test" && sourceIp.isEmpty()) {
        m_lastError = "C2 capture setup failed: test mode requires XB_C2_SOURCE_IP";
        emit captureError(m_lastError);
        return false;
    }
    qInfo().noquote() << "[C2] Mode:" << mode;
    if (mode == "real" && !configuredSource.isEmpty()) {
        qInfo().noquote() << "[C2] Ignoring test source IP in real mode:" << configuredSource;
    }
    if (!sourceIp.isEmpty() && sourceAddress.protocol() != QAbstractSocket::IPv4Protocol) {
        m_lastError = "C2 capture setup failed: XB_C2_SOURCE_IP must be an IPv4 address";
        emit captureError(m_lastError);
        return false;
    }
    if (mode == "real" && realSourceIps.isEmpty()) {
        m_lastError = "C2 capture setup failed: no local IPv4 address on interface " + interface;
        emit captureError(m_lastError);
        return false;
    }
    qInfo().noquote() << "[C2] FC source IP filter:"
                     << (mode == "real" ? realSourceIps.join(',') + " (local XBLink)" : sourceAddress.toString());
#ifdef Q_OS_LINUX
    const unsigned int index = if_nametoindex(interface.toLocal8Bit().constData());
    if (!index) {
        m_lastError = "Unknown C2 capture interface: " + interface;
        emit captureError(m_lastError);
        return false;
    }
    m_fd = ::socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ALL));
    if (m_fd < 0) {
        m_lastError = "C2 AF_PACKET capture failed (root or CAP_NET_RAW required): " + QString::fromLocal8Bit(std::strerror(errno));
        emit captureError(m_lastError);
        return false;
    }
    // Only copy untagged IPv4 UDP/3345 into userspace. VLAN frames are allowed
    // through and validated by the decoder (including nested VLAN tags).
    sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_IP, 3, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_8021Q, 13, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_8021AD, 12, 0),
        BPF_STMT(BPF_RET | BPF_K, 0),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 23),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_UDP, 0, 8),
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 20),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x3fff, 6, 0),
        BPF_STMT(BPF_LDX | BPF_B | BPF_MSH, 14),
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 14),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, port, 4, 0),
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 16),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, port, 2, 0),
        BPF_STMT(BPF_RET | BPF_K, 0),
        BPF_STMT(BPF_RET | BPF_K, 0),
        BPF_STMT(BPF_RET | BPF_K, 65535)
    };
    sock_fprog program{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
    sockaddr_ll address{};
    address.sll_family = AF_PACKET; address.sll_protocol = htons(ETH_P_ALL); address.sll_ifindex = int(index);
    if ((m_useKernelFilter && ::setsockopt(m_fd, SOL_SOCKET, SO_ATTACH_FILTER, &program, sizeof(program)) < 0) ||
        ::bind(m_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        const QString error = QString::fromLocal8Bit(std::strerror(errno));
        closeCapture();
        m_lastError = "C2 capture setup failed: " + error;
        emit captureError(m_lastError);
        return false;
    }
    const int receiveBuffer = 4 * 1024 * 1024;
    ::setsockopt(m_fd, SOL_SOCKET, SO_RCVBUF, &receiveBuffer, sizeof(receiveBuffer));
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, [this]() { drain(); });
    qInfo().noquote() << QString("[C2] Native capture on %1 UDP/%2 (FC %3/%4, BPF=%5, timer drain enabled)")
                        .arg(interface).arg(port).arg(C2_TELEMETRY_FC_SYSTEM_ID).arg(C2_TELEMETRY_FC_COMPONENT_ID)
                        .arg(m_useKernelFilter ? "ON" : "OFF");
    return true;
#else
    Q_UNUSED(interface); Q_UNUSED(port);
    m_lastError = "Native C2 capture requires Linux; the decoder remains available for offline tests.";
    emit captureError(m_lastError);
    return false;
#endif
}

void XbTelemetryHandler::drain()
{
#ifdef Q_OS_LINUX
    // Bound each callback so camera QoS, SIYI and watchdog timers can run.
    char buffer[65536];
    for (int count = 0; count < 256 && m_fd >= 0; ++count) {
        const ssize_t length = ::recv(m_fd, buffer, sizeof(buffer), 0);
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            const QString error = QString::fromLocal8Bit(std::strerror(errno));
            closeCapture();
            m_lastError = "C2 receive failed: " + error;
            emit captureError(m_lastError);
            break;
        }
        if (length) {
            ++m_readPackets;
            m_decoder->ingest(QByteArray(buffer, int(length)), m_clock.elapsed());
        }
    }
#endif
}

void XbTelemetryHandler::publish()
{
    if (!m_decoder) return;
    // Read directly as the working Python collector does. This also provides
    // progress if a target's Qt socket notifier does not activate for AF_PACKET.
    drain();
    quint64 kernelPackets = 0, kernelDrops = 0;
#ifdef Q_OS_LINUX
    if (m_fd >= 0) {
        tpacket_stats stats{};
        socklen_t size = sizeof(stats);
        if (::getsockopt(m_fd, SOL_PACKET, PACKET_STATISTICS, &stats, &size) == 0) {
            kernelPackets = stats.tp_packets;
            kernelDrops = stats.tp_drops;
            m_decoder->addCaptureDrops(stats.tp_drops); // Kernel resets counters on read.
        }
    }
#endif
    auto samples = m_fd >= 0 ? m_decoder->snapshot(m_clock.elapsed()) : QVector<C2TelemetryStat>();
    if (m_debug) {
        qInfo().noquote() << QString("[C2 DEBUG] if=%1 capture=%2 BPF=%3 | window: kernel=%4 read=%5 drops=%6 | %7%8")
            .arg(m_interface, m_fd >= 0 ? "OPEN" : "CLOSED", m_useKernelFilter ? "ON" : "OFF")
            .arg(kernelPackets).arg(m_readPackets).arg(kernelDrops)
            .arg(m_decoder->diagnosticSummary(), m_lastError.isEmpty() ? QString() : " | error=" + m_lastError);
    }
    m_readPackets = 0;
    if (samples.isEmpty()) {
        C2TelemetryStat missing;
        missing.captureAvailable = m_fd >= 0;
        samples.append(missing);
    }
    // Normal telemetry is combined with camera QoS/SIYI by the ABR heartbeat.
    for (const auto &sample : samples) {
        if (m_debug) {
            qInfo().noquote() << QString("[C2 SESSION] source=%1 destination=%2 socket=%3 FC=%4 HB=%5 ACK=%6 RTT_valid=%7 baseline=%8 observation=%9 ready=%10 RTT=%11ms ACK_age=%12ms")
                .arg(sample.source, sample.destination).arg(sample.socketId)
                .arg(sample.telemetryValid).arg(sample.heartbeatValid).arg(sample.peerAckValid)
                .arg(sample.rttValid).arg(sample.baselineValid).arg(sample.observationOk)
                .arg(sample.inputsReady()).arg(sample.rttSmoothedMs, 0, 'f', 1).arg(sample.ackAgeMs);
        }
    }
    emit telemetryUpdated(samples);
}

void XbTelemetryHandler::closeCapture()
{
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
#ifdef Q_OS_LINUX
    if (m_fd >= 0) ::close(m_fd);
#endif
    m_fd = -1;
}

void XbTelemetryHandler::stop()
{
    m_timer.stop();
    closeCapture();
    m_decoder.reset();
}

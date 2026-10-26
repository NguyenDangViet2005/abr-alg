#include "XbTelemetryHandler.h"
#include <QSocketNotifier>
#include <QDateTime>
#include <QDebug>
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
    m_clock.start();
    m_decoder.reset(new C2TelemetryDecoder(port, C2_TELEMETRY_FC_SYSTEM_ID,
        C2_TELEMETRY_FC_COMPONENT_ID, C2_TELEMETRY_STALE_MS));
    m_timer.start(); // Keep emitting unavailable samples if capture cannot start.
#ifdef Q_OS_LINUX
    const unsigned int index = if_nametoindex(interface.toLocal8Bit().constData());
    if (!index) { emit captureError("Unknown C2 capture interface: " + interface); return false; }
    m_fd = ::socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ALL));
    if (m_fd < 0) {
        emit captureError("C2 AF_PACKET capture failed (root or CAP_NET_RAW required): " + QString::fromLocal8Bit(std::strerror(errno)));
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
    if (::setsockopt(m_fd, SOL_SOCKET, SO_ATTACH_FILTER, &program, sizeof(program)) < 0 ||
        ::bind(m_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        const QString error = QString::fromLocal8Bit(std::strerror(errno));
        closeCapture();
        emit captureError("C2 capture setup failed: " + error);
        return false;
    }
    const int receiveBuffer = 4 * 1024 * 1024;
    ::setsockopt(m_fd, SOL_SOCKET, SO_RCVBUF, &receiveBuffer, sizeof(receiveBuffer));
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, [this]() { drain(); });
    qInfo().noquote() << QString("[C2] Native capture on %1 UDP/%2 (FC %3/%4)")
                        .arg(interface).arg(port).arg(C2_TELEMETRY_FC_SYSTEM_ID).arg(C2_TELEMETRY_FC_COMPONENT_ID);
    return true;
#else
    Q_UNUSED(interface); Q_UNUSED(port);
    emit captureError("Native C2 capture requires Linux; the decoder remains available for offline tests.");
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
            emit captureError("C2 receive failed: " + error);
            break;
        }
        if (length) m_decoder->ingest(QByteArray(buffer, int(length)), m_clock.elapsed());
    }
#endif
}

void XbTelemetryHandler::publish()
{
    if (!m_decoder) return;
#ifdef Q_OS_LINUX
    if (m_fd >= 0) {
        tpacket_stats stats{};
        socklen_t size = sizeof(stats);
        if (::getsockopt(m_fd, SOL_PACKET, PACKET_STATISTICS, &stats, &size) == 0)
            m_decoder->addCaptureDrops(stats.tp_drops); // Kernel resets counters on read.
    }
#endif
    auto samples = m_fd >= 0 ? m_decoder->snapshot(m_clock.elapsed()) : QVector<C2TelemetryStat>();
    if (samples.isEmpty()) {
        C2TelemetryStat missing;
        missing.captureAvailable = m_fd >= 0;
        samples.append(missing);
    }
    const QString time = QDateTime::currentDateTime().toString("HH:mm:ss");
    for (const auto &sample : samples) {
        if (!sample.telemetryValid) {
            qInfo().noquote() << QString("[%1] C2 NO SIGNAL ...").arg(time);
        } else {
            qInfo().noquote() << QString("[%1] C2 %2 | RX_APP=%3 kBps | MAV=%4 KiB/s | frames=%5/s | age=%6ms | HB=%7ms")
                .arg(time, sample.destination).arg(sample.appRxModelKiBps, 0, 'f', 2)
                .arg(sample.mavlinkKiBps, 0, 'f', 2).arg(sample.mavlinkFramesPerSecond, 0, 'f', 1)
                .arg(sample.telemetryAgeMs).arg(sample.heartbeatAgeMs);
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

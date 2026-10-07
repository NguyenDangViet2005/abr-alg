#include "QoSDebugSender.h"
#include <QUdpSocket>
#include <QDateTime>
#include <QDir>
#include <QDebug>
#include <QDataStream>

// ── Rose of the C2-mode / branch / reason enums, for the qDebug line ────────
// Kept as free arrays so the log text and the wire enum share one ordering.

namespace {

const char *c2ModeText(int m)
{
    switch (m) {
    case 0: return "FRESH";
    case 1: return "STALE";
    case 2: return "NEVER_SEEN";
    case 3: return "DELAY_ONLY";
    default: return "?";
    }
}

const char *branchText(int b)
{
    switch (b) {
    case QS_BR_HOLD: return "HOLD";
    case QS_BR_UP: return "UP";
    case QS_BR_HALVE: return "HALVE";
    case QS_BR_MUL: return "MUL";
    case QS_BR_PAUSE: return "PAUSE";
    case QS_BR_NOT_AVAILABLE: return "N/A";
    case QS_BR_TO_STUCK_OBSERVE: return "STUCK_OBSERVE";
    case QS_BR_RESET_REQUEST: return "RESET_REQUEST";
    default: return "?";
    }
}

const char *reasonText(int r)
{
    switch (r) {
    case QS_R_NONE: return "NONE";
    case QS_R_KTO: return "KTO";
    case QS_R_LOSS_HIGH: return "LOSS_HIGH";
    case QS_R_QDELAY_400_RESERVED: return "RESERVED_3";
    case QS_R_QDELAY_5_RESERVED: return "RESERVED_4";
    case QS_R_TREND_RAPID_DEC: return "TREND_RAPID_DEC";
    case QS_R_LINK_DEAD: return "LINK_DEAD";
    case QS_R_NOT_USABLE: return "NOT_USABLE";
    case QS_R_STALE: return "STALE";
    case QS_R_COOLDOWN_OK: return "COOLDOWN_OK";
    case QS_R_C2_BLOCK_UP: return "C2_BLOCK_UP";
    case QS_R_SCORE_LOW: return "SCORE_LOW";
    case QS_R_STUCK_OBSERVE: return "STUCK_OBSERVE";
    case QS_R_RESET_STUCK: return "RESET_STUCK";
    case QS_R_C2_LOSS_HIGH: return "C2_LOSS_HIGH";
    case QS_R_SCORE_HIGH: return "SCORE_HIGH";
    case QS_R_CEILING: return "CEILING";
    default: return "?";
    }
}

const char *eventText(int e)
{
    switch (e) {
    case QS_EVT_ROUTE_SWITCH: return "ROUTE_SWITCH";
    case QS_EVT_ABR_PAUSE: return "ABR_PAUSE";
    case QS_EVT_ABR_RESUME: return "ABR_RESUME";
    case QS_EVT_C2_STATE_CHANGE: return "C2_STATE_CHANGE";
    case QS_EVT_STALE_WARN: return "STALE_WARN";
    case QS_EVT_SOCKET_RESET_REQUEST: return "SOCKET_RESET_REQUEST";
    default: return "?";
    }
}

// 8 seconds, chosen to match the drop-log cadence elsewhere in the service.
constexpr qint64 DROP_LOG_INTERVAL_MS = 8000;

// Build token: printed by startUdp() so a log PROVES which revision is running.
// Bump on every change to the QS send path. If a log's "[QSDBG] UDP →" line has
// no build=... tag, or a stale one, the binary predates the fix.
constexpr char QSDBG_BUILD_TAG[] = "noisopen-2026-10-02";

} // namespace

QoSDebugSender::QoSDebugSender(QObject *parent)
    : QObject(parent)
{
    m_clock.start();
    m_dropLog.start();
}

QoSDebugSender::~QoSDebugSender()
{
    stopFileLog();
    stopUdp();
}

// ── Enable / start / stop ───────────────────────────────────────────────────

void QoSDebugSender::setEnabled(bool on)
{
    if (m_enabled == on) return;
    m_enabled = on;
    qInfo().nospace() << "[QSDBG] channel " << (on ? "ENABLED" : "DISABLED");
}

void QoSDebugSender::setTopology(const QVector<QPair<QString, quint8>> &ifaces,
                                 const QString &primaryIface, quint16 udpPort)
{
    m_topology = ifaces;
    m_primaryIface = primaryIface;
    m_port = udpPort;
}

bool QoSDebugSender::startUdp(const QString &host, quint16 port)
{
    if (m_udp) return true;
    m_host = QHostAddress(host);
    if (m_host.isNull()) {
        qWarning().nospace() << "[QSDBG] invalid host '" << host << "' — UDP disabled";
        return false;
    }
    m_port = port;
    m_udp = new QUdpSocket(this);
    // No bind()/connectToHost() on purpose. A plain UDP sender is not "open" by
    // QIODevice semantics (isOpen() == false) because nothing called open() —
    // gating the send on isOpen() would silently drop every record. writeDatagram
    // on an unbound socket is valid: the kernel assigns an ephemeral port and
    // sends. The build tag below proves which revision produced this log.
    qInfo().nospace() << "[QSDBG] UDP → " << host << ":" << port
                      << " build=" << QSDBG_BUILD_TAG;
    return true;
}

void QoSDebugSender::stopUdp()
{
    if (!m_udp) return;
    m_udp->close();
    delete m_udp;
    m_udp = nullptr;
}

void QoSDebugSender::startFileLog(const QString &dir)
{
    if (m_file.isOpen()) return;
    if (dir.isEmpty()) {
        qWarning().noquote() << "[QSDBG] file log requested but dir empty — skipped";
        return;
    }
    QDir d(dir);
    if (!d.exists() && !d.mkpath(".")) {
        qWarning().nospace() << "[QSDBG] cannot create log dir '" << dir << "'";
        return;
    }
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    const QString path = d.filePath(QStringLiteral("qos-session-%1.qsdb").arg(stamp));
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning().nospace() << "[QSDBG] cannot open '" << path << "': " << m_file.errorString();
        return;
    }
    // Header: 'QSDB' + version + placeholder count (rewritten at close).
    QByteArray header;
    QDataStream ds(&header, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.writeRawData(QSDB_MAGIC, 4);
    ds << QSDB_VERSION;
    ds << static_cast<quint32>(0);
    m_file.write(header);
    m_file.flush();
    m_fileCount = 0;
    qInfo().nospace() << "[QSDBG] file log → " << path;
}

void QoSDebugSender::stopFileLog()
{
    if (!m_file.isOpen()) return;
    // Rewrite the record count at offset 5 (after magic(4) + version(1)).
    m_file.flush();
    m_file.seek(5);
    QByteArray count;
    QDataStream ds(&count, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << m_fileCount;
    m_file.write(count);
    m_file.flush();
    m_file.close();
    qInfo().nospace() << "[QSDBG] file log closed (" << m_fileCount << " records)";
    m_fileCount = 0;
}

// ── qDebug formatting (always on) ───────────────────────────────────────────

void QoSDebugSender::logSample(const QsSample &s) const
{
    const auto fx = [](double v, int p) { return QString::number(v, 'f', p); };
    qDebug().nospace()
        << "[QS][" << s.iface << "]"
        << " seq=" << s.tickSeq
        << " prio=" << s.priority
        << " flags[act=" << s.isActivated << " conn=" << s.isConnected
        << " mptcp=" << s.isMptcp << " usable=" << s.usable << "]"
        << " pres[v=" << s.videoPresent << " c2=" << s.c2Present << "]"
        << " trend[v=" << s.videoTrend << " c2=" << s.c2Trend << "]"
        << " c2Mode=" << c2ModeText(s.c2Mode)
        << " abr=" << branchText(s.abrBranch) << "/" << reasonText(s.abrReason)
        << " br=" << s.bitrateKbps << "kbps"
        << " max=" << s.maxAbrKbps
        << " V[score=" << fx(s.video.score, 1) << " thr=" << fx(s.video.thr, 0)
        << " loss=" << fx(s.video.loss, 3) << " rtt=" << fx(s.video.rtt, 1)
        << " qdelay=" << fx(s.video.qdelay, 1) << " jit=" << fx(s.video.jitter, 1) << "]"
        << " C2[score=" << fx(s.c2.score, 1) << " thr=" << fx(s.c2.thr, 0)
        << " loss=" << fx(s.c2.loss, 3) << " rtt=" << fx(s.c2.rtt, 1)
        << " qdelay=" << fx(s.c2.qdelay, 1) << " jit=" << fx(s.c2.jitter, 1) << "]";
}

void QoSDebugSender::logEvent(const QsEvent &e) const
{
    qDebug().nospace() << "[QS][EVENT][" << e.iface << "] "
                       << eventText(e.eventType) << " detail=\"" << e.detail << "\"";
}

// ── Row accumulation ────────────────────────────────────────────────────────

void QoSDebugSender::handleRouterRow(const RouterInterfaceStat &row)
{
    if (row.parseFailed) return;   // zeros would read as measured — drop it
    if (row.connType == ConnType::C2) {
        m_c2.insert(row.interfaceName, row);
    } else {
        m_video.insert(row.interfaceName, row);
    }
    // The per-row qDebug already happens in ABRFactory::handleRouterRow; this
    // class logs the MERGED per-tick view in logSample, which is the one that
    // shows input beside decision. No duplicate row line here on purpose.
}

void QoSDebugSender::handleTick(const QString &primaryInterface)
{
    m_primaryIface = primaryInterface;

    // Every interface that reported a VIDEO row this tick. A tick with only a
    // C2 row (video 600 lost) would be skipped — the ABR engine has the same
    // freshness rule, so the two stay consistent.
    const QStringList ifaces = m_video.keys();
    for (const QString &iface : ifaces) {
        emitSample(iface, primaryInterface);
    }

    m_video.clear();
    m_c2.clear();
    m_seq++;
}

QsSample QoSDebugSender::buildSample(const QString &iface) const
{
    const RouterInterfaceStat v = m_video.value(iface);
    const RouterInterfaceStat c = m_c2.value(iface);
    const bool c2Present = m_c2.contains(iface);
    const AbrState abr = m_decision.value(iface);

    QsSample s;
    s.timestampMs = QDateTime::currentMSecsSinceEpoch();
    s.tickSeq = m_seq;
    s.iface = iface;
    s.priority = static_cast<quint8>(v.priority);

    s.isActivated = v.isActivated;
    s.isConnected = v.isConnected;
    s.isMptcp     = v.isMptcp;
    s.usable      = v.usable;

    s.videoPresent = true;
    s.c2Present    = c2Present;

    s.videoTrend = static_cast<quint8>(v.trend);
    s.c2Trend    = c2Present ? static_cast<quint8>(c.trend) : 255;
    // Quality is not on the router's 22-field row (it is derived); 255 = absent
    // so the dashboard renders a dash rather than a fabricated 0.
    s.videoQuality = 255;
    s.c2Quality    = 255;

    s.c2Mode = abr.present ? static_cast<quint8>(abr.c2Mode) : 0;
    s.abrBranch = abr.present ? static_cast<quint8>(abr.branch) : QS_BR_NOT_AVAILABLE;
    s.abrReason = abr.present ? static_cast<quint8>(abr.reason) : QS_R_NONE;
    s.notUsableTicks = 0;

    s.bitrateKbps       = abr.bitrateKbps;
    s.maxAbrKbps        = abr.maxAbrKbps;
    s.sinceLastChangeMs = 0;

    // thr/rtt/minRtt are METHODS on RouterInterfaceStat: the wire carries only
    // the raw counters (µs, B/s) and these convert them for display, so they are
    // called, not read as fields.
    s.video.score  = v.score;
    s.video.thr    = v.throughputKbps();
    s.video.loss   = v.lossRatePct;
    s.video.rtt    = v.rttMs();
    s.video.minRtt = v.minRttMs();
    s.video.qdelay = v.queuingDelayMs;
    s.video.jitter = v.jitterMs;

    s.c2.score  = c.score;
    s.c2.thr    = c.throughputKbps();
    s.c2.loss   = c.lossRatePct;
    s.c2.rtt    = c.rttMs();
    s.c2.minRtt = c.minRttMs();
    s.c2.qdelay = c.queuingDelayMs;
    s.c2.jitter = c.jitterMs;

    return s;
}

void QoSDebugSender::emitSample(const QString &iface, const QString &primaryIface)
{
    Q_UNUSED(primaryIface);   // the 602 name is carried per-interface; not needed per sample
    const QsSample s = buildSample(iface);
    logSample(s);

    if (!m_enabled) return;
    writeRecord(QS_SAMPLE, buildQsDatagram(QS_SAMPLE, encodeQsSample(s)));
}

// ── ABR decision hook ───────────────────────────────────────────────────────

void QoSDebugSender::handleAbrDecision(const QString &iface, int kbps, int c2Mode,
                                       int branch, int reason, int maxAbr)
{
    AbrState &st = m_decision[iface];
    st.present = true;
    st.bitrateKbps = kbps;
    st.c2Mode = c2Mode;
    st.branch = branch;
    st.reason = reason;
    st.maxAbrKbps = maxAbr;
}

void QoSDebugSender::handleEvent(const QString &iface, quint8 type, const QString &detail)
{
    QsEvent e;
    e.timestampMs = QDateTime::currentMSecsSinceEpoch();
    e.iface = iface;
    e.eventType = type;
    e.detail = detail;

    logEvent(e);
    if (!m_enabled) return;
    writeRecord(QS_EVENT, buildQsDatagram(QS_EVENT, encodeQsEvent(e)));
}

void QoSDebugSender::emitConfig()
{
    if (!m_enabled) return;
    QsConfig c;
    c.appVersion = m_appVersion;
    c.primaryIface = m_primaryIface;
    c.udpPort = m_port;
    c.ifaces = m_topology;
    writeRecord(QS_CONFIG, buildQsDatagram(QS_CONFIG, encodeQsConfig(c)));
}

// ── Publish ─────────────────────────────────────────────────────────────────

void QoSDebugSender::writeRecord(quint8 recordType, const QByteArray &datagram)
{
    // UDP — log at the send boundary, the only place that proves a record LEFT
    // the socket. logSample()/logEvent() run BEFORE the `m_enabled` guard, so a
    // "[QS] ..." line in the log proves the record was BUILT, never that it was
    // sent. This line prints the resolved host:port, byte count and wire magic
    // for every record, so "dashboard sees nothing" splits cleanly into
    // "never sent" vs "sent but not received" (wrong host / firewall / not bound).
    //
    // Guard on m_udp alone, NOT isOpen(): a fresh QUdpSocket that never called
    // bind()/connectToHost() has openMode==NotOpen, so isOpen() is false and
    // gating on it dropped every record silently. writeDatagram() on an unbound
    // socket is valid and auto-binds an ephemeral source port.
    if (m_udp) {
        const qint64 n = m_udp->writeDatagram(datagram, m_host, m_port);
        const QString magic = datagram.size() >= 2
            ? QString("%1 %2")
                  .arg(static_cast<quint8>(datagram.at(0)), 2, 16, QChar('0'))
                  .arg(static_cast<quint8>(datagram.at(1)), 2, 16, QChar('0'))
            : QStringLiteral("--");
        if (n < 0) {
            // qInfo, not qDebug: the "UDP →" proof line is qInfo, so the send
            // result must match its visibility or a suppressed qDebug would
            // make a working send look like a silent drop again.
            qWarning().nospace() << "[QSDBG] UDP send FAILED -> " << m_host.toString()
                                 << ":" << m_port << " type=" << recordType
                                 << " bytes=" << datagram.size()
                                 << " magic=" << magic
                                 << " err=" << m_udp->errorString();
            if (m_dropLog.elapsed() >= DROP_LOG_INTERVAL_MS) {
                m_dropLog.restart();
                emit onDropped(recordType, static_cast<quint16>(datagram.size()));
            }
        } else {
            qInfo().nospace() << "[QSDBG] UDP sent " << n << " bytes -> "
                              << m_host.toString() << ":" << m_port
                              << " type=" << recordType << " magic=" << magic;
        }
    } else {
        // m_udp == nullptr: startUdp never ran. Warn once per window.
        if (m_dropLog.elapsed() >= DROP_LOG_INTERVAL_MS) {
            m_dropLog.restart();
            qWarning().nospace() << "[QSDBG] UDP NOT sent (socket null) -> "
                                 << m_host.toString() << ":" << m_port
                                 << " type=" << recordType << " bytes=" << datagram.size();
            emit onDropped(recordType, static_cast<quint16>(datagram.size()));
        }
    }

    // File: [u32 len][datagram], little-endian, flushed per record (debug only).
    if (m_file.isOpen()) {
        QByteArray framed;
        QDataStream ds(&framed, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::LittleEndian);
        ds << static_cast<quint32>(datagram.size());
        ds.writeRawData(datagram.constData(), datagram.size());
        m_file.write(framed);
        m_file.flush();
        m_fileCount++;
    }
}

#include "RouterNetworkHandler.h"
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QDebug>

// Wire shape (shared with XBFirm's telemetry port):
//   byte 0..1 = datagram id, "60" for the router's per-interface metrics
//   byte 2    = ConnType (row kind), see RouterInterfaceStat.h
//   byte 3..  = payload, '#'-separated
static const int DATAGRAM_ID_LEN = 2;
static const char ROUTER_ID[] = "60";

// The router sends ALL of its telemetry to one destination from one socket:
// TelemetryModule::sendLoop() emits 550, optionally 560, then 600/601/602 on
// the same _sockfd with the same _servAddr. So these two ids arrive on 12346 as
// well and are EXPECTED here — they are the router being healthy, not a
// misdirected sender.
//
// They are dropped deliberately: 550/560 are the legacy 10-field video stub
// (TelemetryModule::formatVideoData), not per-interface rows, and nothing on
// this port consumes them. Dropping them is fine; dropping them SILENTLY is
// not, so each is counted and reported once per window rather than per packet.
static const char SPECTRUM_STUB_ID[]  = "55";   // 550
static const char SPECTRUM_STUB2_ID[] = "56";   // 560

// A 1 Hz router makes this at most one line per window. Not a rate limiter in
// the flood-control sense — there is simply nothing here worth logging 2x/s.
static const int UNKNOWN_ID_LOG_INTERVAL_SEC = 10;

// VIDEO / C2 rows carry exactly this many fields, positionally. The order is
// the wire contract: fields 7..16 are the RAW tcpi counter block in the OLD
// branch's units (xbfirm's processTCPInfo inputs), fields 17..22 are the
// refactor's derived metrics. Changing one without the others shifts every
// value; see RouterInterfaceStat.h and docs/xblink-telemetry-protocol.md §3b.
static const int ROW_FIELD_COUNT = 22;
static const char *const ROW_FIELD_NAMES[ROW_FIELD_COUNT] = {
    "interfaceName", "priority",     "isActivated",   "isConnected",
    "isMptcp",       "usable",
    "rttUs",         "deliveryRateBps", "lost",       "segsOut",
    "sndCwnd",       "sndMss",       "lastAckRecv",   "sockOutq",
    "sockSndBuf",    "minRttUs",
    "score",         "trend",        "lossRatePct",   "queuingDelayMs",
    "jitterMs",      "sockOutqUnacked"
};

// Parse one numeric field, reporting a conversion failure rather than
// silently yielding 0.
//
// QString::toInt()/toDouble() return 0 and discard the reason, so a field the
// router sent as "abc" or "1569,76" (a locale decimal comma) would become a
// plausible 0.00. The `ok` out-param is the only way to tell those apart, and
// the offending TEXT is printed because the two cases mean different things.
static int parseIntField(const QStringList &fields, int index, bool *anyFailure)
{
    bool ok = false;
    const QString raw = fields.at(index);
    const int value = raw.toInt(&ok);
    if (!ok) {
        qWarning().nospace() << "[ROUTER][PARSE] field " << index << " ('" << raw
                             << "') unparsable as int — treated as 0";
        *anyFailure = true;
    }
    return value;
}

static double parseDoubleField(const QStringList &fields, int index, bool *anyFailure)
{
    bool ok = false;
    const QString raw = fields.at(index);
    const double value = raw.toDouble(&ok);
    if (!ok) {
        qWarning().nospace() << "[ROUTER][PARSE] field " << index << " ('" << raw
                             << "') unparsable as double — treated as 0";
        *anyFailure = true;
    }
    return value;
}

// Byte counters (fields 14/15) are emitted by the router as unsigned integers
// via std::to_string(uint64_t). Parsed as quint64 so a value above INT_MAX
// (a 4 GiB+ backlog) is not truncated to a negative int.
static quint64 parseU64Field(const QStringList &fields, int index, bool *anyFailure)
{
    bool ok = false;
    const QString raw = fields.at(index);
    const quint64 value = raw.toULongLong(&ok);
    if (!ok) {
        qWarning().nospace() << "[ROUTER][PARSE] field " << index << " ('" << raw
                             << "') unparsable as uint64 — treated as 0";
        *anyFailure = true;
    }
    return value;
}

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
    const int minLen = DATAGRAM_ID_LEN + 1;   // id + connType byte
    if (data.size() < minLen) {
        // A datagram this short cannot be dispatched at all. Without this line a
        // truncated send would be indistinguishable from "the router never sent
        // it" — the exact ambiguity that made the id-60 path invisible before.
        qWarning().nospace() << "[ROUTER][RX] datagram too short (" << data.size()
                             << " bytes, need >= " << minLen << ") from "
                             << sender.toString() << ":" << senderPort
                             << " raw=" << QString::fromLatin1(data);
        return;
    }

    const QByteArray id = data.left(DATAGRAM_ID_LEN);
    if (id != ROUTER_ID) {
        // Two distinct cases, and they must not read the same in the log:
        //
        //  550/560 — the router's own legacy video stub, expected on this port
        //            because the router sends every telemetry id to one
        //            destination from one socket. Nothing here consumes it, so
        //            it is ignored — but silently ignoring it is exactly the
        //            failure this project already spent days chasing, so it is
        //            still named once per window.
        //
        //  anything else — a foreign sender on a port that takes datagrams
        //            from anyone on the segment. Warned, not ignored.
        const bool isRouterStub = (id == SPECTRUM_STUB_ID || id == SPECTRUM_STUB2_ID);
        if (isRouterStub) {
            if (_lastStubLog.isValid() && _lastStubLog.elapsed() < UNKNOWN_ID_LOG_INTERVAL_SEC * 1000) {
                return;
            }
            _lastStubLog.restart();
            qDebug().nospace() << "[ROUTER][RX] id=" << QString::fromLatin1(id)
                               << " is the router's legacy video stub, not a per-interface row"
                               << " — ignoring (logged at most once per "
                               << UNKNOWN_ID_LOG_INTERVAL_SEC << "s)"
                               << " from " << sender.toString() << ":" << senderPort;
            return;
        }

        qWarning().nospace() << "[ROUTER][RX] unexpected id=" << QString::fromLatin1(id)
                             << " on the telemetry port from "
                             << sender.toString() << ":" << senderPort
                             << " raw=" << QString::fromLatin1(data);
        return;
    }

    const int connTypeRaw = data.mid(DATAGRAM_ID_LEN, 1).toInt();
    const QString payload = QString::fromUtf8(data.mid(DATAGRAM_ID_LEN + 1)).trimmed();
    const QStringList fields = payload.split('#');

    if (connTypeRaw < 0 || connTypeRaw > 4) {
        // A connType outside the defined values means the router and this parser
        // have diverged. Logged distinctly but parsing continues so the raw
        // payload is visible to diagnose the mismatch.
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
        // PRIMARY's payload is a NAME, not a metric row — its arity is
        // legitimately 1, so it never goes through emitRow's 22-field path.
        const QString primary = fields.value(0, QStringLiteral("-"));
        if (fields.size() != 1) {
            qWarning().nospace() << "[ROUTER][RX] PRIMARY expected 1 field, got "
                                 << fields.size() << " — using '" << primary << "'";
        }
        qDebug().nospace() << "[ROUTER][RX]   primaryInterface=" << primary;
        emit onRouterTick(primary);
        return;
    }

    if (connType == ConnType::NETWORK_REF) {
        // NETWORK_REF's payload is <name>#<strength> — a 2-field packet, so like
        // PRIMARY it self-returns and never enters emitRow's 22-field path.
        // A malformed strength is fail-closed to 9 (no info), so a truncated 603
        // can never cause a spurious ceiling lift.
        const QString name = fields.value(0);
        bool ok = false;
        int strength = fields.value(1).toInt(&ok);
        if (!ok) {
            qWarning().nospace() << "[ROUTER][RX] NETWORK_REF bad strength '"
                                 << fields.value(1) << "' — defaulting to 9 (no info)";
            strength = 9;
        }
        qDebug().nospace() << "[ROUTER][RX]   networkStrength name=" << name
                           << " strength=" << strength;
        emit onNetworkStrength(name, strength);
        return;
    }

    if (connType == ConnType::GPS) {
        // GPS's payload is <lat>#<lon> — a 2-field packet, GLOBAL (no name), so
        // like PRIMARY and NETWORK_REF it self-returns and never enters emitRow's
        // 22-field path. A malformed fix is fail-closed: no emit at all, so the
        // consumer's staleness timer reads it as "no fix" (never a bogus 0,0).
        bool okLat = false, okLon = false;
        const double lat = fields.value(0).toDouble(&okLat);
        const double lon = fields.value(1).toDouble(&okLon);
        if (!okLat || !okLon) {
            qWarning().nospace() << "[ROUTER][RX] GPS bad fix '"
                                 << fields.value(0) << "#" << fields.value(1)
                                 << "' — dropped (no emit)";
            return;
        }
        qDebug().nospace() << "[ROUTER][RX]   gpsFix lat=" << lat << " lon=" << lon;
        emit onGpsFix(lat, lon);
        return;
    }

    logRowFields(connType, fields);
    emitRow(connType, fields);
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

void RouterNetworkHandler::emitRow(ConnType connType, const QStringList &fields)
{
    // A short row is still emitted, with the missing fields left at their
    // defaults and parseFailed set. Dropping it instead would make a
    // systematically truncated stream look like an idle one.
    if (fields.size() < ROW_FIELD_COUNT) {
        qWarning().nospace() << "[ROUTER][RX] connType=" << static_cast<int>(connType)
                             << " short row (" << fields.size() << "/" << ROW_FIELD_COUNT
                             << ") — emitting partial, will not be acted on";
    }

    RouterInterfaceStat row;
    row.connType = connType;
    row.interfaceName = fields.value(0);

    // Anything short of a full row cannot be trusted positionally: field 5 of a
    // 6-field row is not necessarily `usable`. Emit the identity and the flag,
    // leave the numbers at 0, and let the consumer skip it.
    if (fields.size() < ROW_FIELD_COUNT) {
        row.parseFailed = true;
        emit onRouterRow(row);
        return;
    }

    bool failed = false;
    // #1..#6 — identity / status.
    row.priority    = parseIntField(fields, 1, &failed);
    row.isActivated = parseIntField(fields, 2, &failed) != 0;
    row.isConnected = parseIntField(fields, 3, &failed) != 0;
    row.isMptcp     = parseIntField(fields, 4, &failed) != 0;
    row.usable      = parseIntField(fields, 5, &failed) != 0;
    // #7..#16 — the RAW tcpi counter block, OLD-branch units (µs, B/s, packet
    // counts). deliveryRateBps is a byte/s rate that can exceed INT_MAX on a
    // fast link, so it is parsed as u64 like the two byte counters below.
    row.rttUs           = parseIntField(fields, 6, &failed);
    row.deliveryRateBps = parseU64Field(fields, 7, &failed);
    row.lost            = parseIntField(fields, 8, &failed);
    row.segsOut         = parseIntField(fields, 9, &failed);
    row.sndCwnd         = parseIntField(fields, 10, &failed);
    row.sndMss          = parseIntField(fields, 11, &failed);
    row.lastAckRecv     = parseIntField(fields, 12, &failed);
    row.sockOutq        = parseIntField(fields, 13, &failed);
    row.sockSndBuf      = parseIntField(fields, 14, &failed);
    row.minRttUs        = parseIntField(fields, 15, &failed);
    // #17..#22 — refactor-derived metrics.
    row.score           = parseDoubleField(fields, 16, &failed);
    row.trend           = parseIntField(fields, 17, &failed);
    row.lossRatePct     = parseDoubleField(fields, 18, &failed);
    row.queuingDelayMs  = parseDoubleField(fields, 19, &failed);
    row.jitterMs        = parseDoubleField(fields, 20, &failed);
    row.sockOutqUnacked = parseU64Field(fields, 21, &failed);
    row.parseFailed     = failed;

    emit onRouterRow(row);
}

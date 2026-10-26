#include "C2TelemetryDecoder.h"
#include "SrtTelemetrySession.h"
#include <QHostAddress>
#include <QtEndian>
#include <map>

using XbTelemetry::Session;

struct C2TelemetryDecoder::Impl {
    quint16 port;
    quint8 systemId, componentId;
    qint64 staleMs, previousTime = -1;
    quint64 order = 0, captureDrops = 0;
    quint64 ethernetFrames = 0, ipv4UdpPackets = 0, c2Packets = 0;
    std::map<QString, Session> sessions;
};

C2TelemetryDecoder::C2TelemetryDecoder(quint16 port, quint8 systemId, quint8 componentId, qint64 staleMs)
    : d(new Impl{})
{
    d->port = port; d->systemId = systemId; d->componentId = componentId; d->staleMs = staleMs;
}
C2TelemetryDecoder::~C2TelemetryDecoder() = default;

void C2TelemetryDecoder::addCaptureDrops(quint64 count) { d->captureDrops += count; }

void C2TelemetryDecoder::ingest(const QByteArray &frame, qint64 now)
{
    ++d->ethernetFrames;
    if (frame.size() < 14) return;
    const auto *p = reinterpret_cast<const uchar *>(frame.constData());
    int offset = 14;
    quint16 type = qFromBigEndian<quint16>(p + 12);
    while (type == 0x8100 || type == 0x88a8) {
        if (frame.size() < offset + 4) return;
        type = qFromBigEndian<quint16>(p + offset + 2);
        offset += 4;
    }
    if (type != 0x0800 || frame.size() < offset + 20) return;
    p += offset;
    if ((p[0] >> 4) != 4 || p[9] != 17) return;
    const int ihl = (p[0] & 15) * 4;
    const int total = qFromBigEndian<quint16>(p + 2);
    if (ihl < 20 || total < ihl + 8 || frame.size() - offset < total || (qFromBigEndian<quint16>(p + 6) & 0x3fff)) return;
    const quint16 sourcePort = qFromBigEndian<quint16>(p + ihl);
    const quint16 destinationPort = qFromBigEndian<quint16>(p + ihl + 2);
    const int length = qFromBigEndian<quint16>(p + ihl + 4);
    if (length < 8 || ihl + length > total) return;
    ++d->ipv4UdpPackets;
    if (length < 24 || (sourcePort != d->port && destinationPort != d->port)) return;
    ++d->c2Packets;
    const quint32 socketId = qFromBigEndian<quint32>(p + ihl + 8 + 12);
    const QString sourceIp = QHostAddress(qFromBigEndian<quint32>(p + 12)).toString();
    const QString destinationIp = QHostAddress(qFromBigEndian<quint32>(p + 16)).toString();
    const QString source = sourceIp + ':' + QString::number(sourcePort);
    const QString destination = destinationIp + ':' + QString::number(destinationPort);
    const QString pairKey = source + '>' + destination;
    const QString key = pairKey + '/' + QString::number(socketId);
    const QByteArray payload(reinterpret_cast<const char *>(p + ihl + 8), length - 8);
    auto it = d->sessions.find(key);
    if (it == d->sessions.end()) {
        if (d->sessions.size() >= 64) {
            auto oldest = d->sessions.begin();
            for (auto current = d->sessions.begin(); current != d->sessions.end(); ++current)
                if (current->second.lastSeen < oldest->second.lastSeen) oldest = current;
            d->sessions.erase(oldest);
        }
        Session session;
        session.source = source; session.destination = destination;
        session.sourceIp = sourceIp; session.destinationIp = destinationIp;
        session.pairKey = pairKey; session.socketId = socketId;
        session.outgoing = sourcePort == d->port;
        session.created = session.lastSeen = now;
        session.order = ++d->order;
        it = d->sessions.emplace(key, std::move(session)).first;
    }
    Session &session = it->second;
    if (session.outgoing && !(quint8(payload[0]) & 0x80) && session.counts.data == 0) {
        // A socket may first appear in control traffic. Rank sessions by their
        // first outgoing data, not by the earlier control-packet allocation.
        session.created = now;
        session.order = ++d->order;
        bool reconnect = false;
        for (const auto &entry : d->sessions)
            if (entry.second.pairKey == pairKey && entry.second.socketId != socketId && entry.second.counts.data)
                reconnect = true;
        if (reconnect) {
            for (auto &entry : d->sessions) {
                auto &peer = entry.second;
                if (peer.source == destination && peer.destination == source) {
                    peer.lastAck = -1;
                    peer.rtt = XbTelemetry::RttTracker();
                    peer.previous = peer.counts;
                }
            }
        }
    }
    if (d->previousTime < 0) d->previousTime = now;
    session.receive(payload, now, d->systemId, d->componentId, d->staleMs);
}

QString C2TelemetryDecoder::diagnosticSummary() const
{
    quint64 data = 0, chunks = 0, mavlink = 0, fc = 0, badCrc = 0, invalid = 0, encrypted = 0;
    int pending = 0;
    const Session *latest = nullptr;
    for (const auto &entry : d->sessions) {
        const auto &session = entry.second;
        if (!session.outgoing) continue;
        data += session.counts.data; chunks += session.counts.chunks;
        mavlink += session.parser.framesValid(); fc += session.counts.frames;
        badCrc += session.parser.crcRejected(); invalid += session.counts.invalid;
        encrypted += session.encryptedPackets; pending += session.parser.pendingBytes();
        if (!latest || session.lastSeen > latest->lastSeen ||
            (session.lastSeen == latest->lastSeen && session.order > latest->order)) latest = &session;
    }
    const QString prefix = QString("total: eth=%1 udp=%2 c2=%3 out_data=%4 chunks=%5 mav_ok=%6 fc=%7 crc_bad=%8 invalid=%9 encrypted=%10 pending_mav=%11")
        .arg(d->ethernetFrames).arg(d->ipv4UdpPackets).arg(d->c2Packets).arg(data).arg(chunks)
        .arg(mavlink).arg(fc).arg(badCrc).arg(invalid).arg(encrypted).arg(pending);
    if (!latest) return prefix;
    return prefix + QString(" | out=%1->%2 last_ids=%3 xb_prefix=%4")
        .arg(latest->source, latest->destination,
             latest->lastMavlinkSource.isEmpty() ? "N/A" : latest->lastMavlinkSource,
             QString::fromLatin1(latest->lastXbPrefix));
}

QVector<C2TelemetryStat> C2TelemetryDecoder::snapshot(qint64 now)
{
    const double elapsed = d->previousTime < 0 ? 0 : qMax(qint64(0), now - d->previousTime) / 1000.0;
    auto age = [now](qint64 time) { return time < 0 ? qint64(-1) : qMax(qint64(0), now - time); };
    auto fresh = [this](qint64 value) { return value >= 0 && value <= d->staleMs; };
    std::map<QString, Session *> newest;
    std::map<const Session *, C2TelemetryStat> rows;
    for (auto &entry : d->sessions) {
        auto &session = entry.second;
        session.flush(now, d->systemId, d->componentId);
        C2TelemetryStat row;
        row.captureAvailable = true;
        row.source = session.source; row.destination = session.destination; row.socketId = session.socketId;
        row.telemetryAgeMs = age(session.lastFc); row.heartbeatAgeMs = age(session.lastHeartbeat);
        row.ackAgeMs = age(session.lastAck);
        row.telemetryValid = fresh(row.telemetryAgeMs); row.heartbeatValid = fresh(row.heartbeatAgeMs);
        row.peerAckValid = fresh(row.ackAgeMs);
        row.windowSeconds = elapsed;
        row.dataPackets = session.counts.data - session.previous.data;
        row.retransmissions = session.counts.retransmits - session.previous.retransmits;
        row.nakReports = session.counts.nak - session.previous.nak;
        row.captureGaps = session.counts.gaps - session.previous.gaps;
        row.captureDrops = d->captureDrops;
        row.observationOk = !row.captureGaps && !row.captureDrops && session.counts.invalid == session.previous.invalid;
        row.crcRejected = session.parser.crcRejected() - session.previousCrcRejected;
        if (row.dataPackets) row.retransmissionPercent = 100.0 * row.retransmissions / row.dataPackets;
        if (elapsed > 0) {
            row.nakReportsPerSecond = row.nakReports / elapsed;
            row.mavlinkFramesPerSecond = (session.counts.frames - session.previous.frames) / elapsed;
            const quint64 bytes = session.counts.bytes - session.previous.bytes;
            const quint64 chunks = session.counts.chunks - session.previous.chunks;
            row.mavlinkKiBps = bytes / elapsed / 1024.0;
            row.appRxModelKiBps = (2 * bytes + chunks) / elapsed / 1024.0;
        }
        session.rtt.snapshot(row, now, d->staleMs, elapsed);
        session.previous = session.counts;
        session.previousCrcRejected = session.parser.crcRejected();
        rows.emplace(&session, row);
        if (session.outgoing && session.counts.data) {
            auto current = newest.find(session.pairKey);
            if (current == newest.end() || current->second->order < session.order) newest[session.pairKey] = &session;
        }
    }
    std::map<QString, QVector<std::pair<Session *, C2TelemetryStat>>> groups;
    for (const auto &entry : newest) {
        auto *session = entry.second;
        C2TelemetryStat row = rows.at(session);
        Session *peer = nullptr;
        for (auto &candidate : d->sessions) {
            auto &value = candidate.second;
            if (value.source == session->destination && value.destination == session->source && value.socketId != 0 &&
                value.lastSeen >= session->created && (!peer || value.lastSeen > peer->lastSeen ||
                    (value.lastSeen == peer->lastSeen && value.order > peer->order))) peer = &value;
        }
        if (peer) {
            const C2TelemetryStat &feedback = rows.at(peer);
            row.peerAckValid = feedback.peerAckValid; row.ackAgeMs = feedback.ackAgeMs;
            row.rttValid = feedback.rttValid; row.rttAgeMs = feedback.rttAgeMs;
            row.baselineValid = feedback.baselineValid;
            row.rttMs = feedback.rttMs; row.rttSmoothedMs = feedback.rttSmoothedMs;
            row.rttBaselineMs = feedback.rttBaselineMs; row.rttInflationMs = feedback.rttInflationMs;
            row.rttRatio = feedback.rttRatio; row.rttTrendValid = feedback.rttTrendValid;
            row.rttTrendMsPerSecond = feedback.rttTrendMsPerSecond;
            row.nakReports = feedback.nakReports; row.nakReportsPerSecond = feedback.nakReportsPerSecond;
            row.observationOk = row.observationOk && feedback.observationOk;
        } else {
            row.peerAckValid = row.rttValid = row.baselineValid = false;
        }
        groups[session->source + '>' + session->destinationIp].append({session, row});
    }
    QVector<C2TelemetryStat> selected;
    for (const auto &group : groups) {
        const auto &candidates = group.second;
        bool anyFresh = false;
        const auto *latest = &candidates.front();
        for (const auto &candidate : candidates) {
            if (candidate.first->order > latest->first->order) latest = &candidate;
            if (candidate.second.telemetryValid) { selected.append(candidate.second); anyFresh = true; }
        }
        if (!anyFresh) selected.append(latest->second);
    }
    d->captureDrops = 0;
    d->previousTime = now;
    return selected;
}
